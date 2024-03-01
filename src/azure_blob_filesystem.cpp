#include "azure_blob_filesystem.hpp"

#include "azure_storage_account_client.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/http_state.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include <azure/storage/blobs.hpp>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace duckdb {

static AzureReadOptions ParseAzureReadOptions(FileOpener *opener) {
	AzureReadOptions options;

	Value concurrency_val;
	if (FileOpener::TryGetCurrentSetting(opener, "azure_read_transfer_concurrency", concurrency_val)) {
		options.transfer_concurrency = concurrency_val.GetValue<int32_t>();
	}

	Value chunk_size_val;
	if (FileOpener::TryGetCurrentSetting(opener, "azure_read_transfer_chunk_size", chunk_size_val)) {
		options.transfer_chunk_size = chunk_size_val.GetValue<int64_t>();
	}

	Value buffer_size_val;
	if (FileOpener::TryGetCurrentSetting(opener, "azure_read_buffer_size", buffer_size_val)) {
		options.buffer_size = buffer_size_val.GetValue<idx_t>();
	}

	return options;
}

// taken from s3fs.cpp TODO: deduplicate!
static bool Match(vector<string>::const_iterator key, vector<string>::const_iterator key_end,
                  vector<string>::const_iterator pattern, vector<string>::const_iterator pattern_end) {

	while (key != key_end && pattern != pattern_end) {
		if (*pattern == "**") {
			if (std::next(pattern) == pattern_end) {
				return true;
			}
			while (key != key_end) {
				if (Match(key, key_end, std::next(pattern), pattern_end)) {
					return true;
				}
				key++;
			}
			return false;
		}
		if (!LikeFun::Glob(key->data(), key->length(), pattern->data(), pattern->length())) {
			return false;
		}
		key++;
		pattern++;
	}
	return key == key_end && pattern == pattern_end;
}

//////// AzureBlobContextState ////////
AzureBlobContextState::AzureBlobContextState(Azure::Storage::Blobs::BlobServiceClient client,
                                             const AzureReadOptions &azure_read_options)
    : read_options(azure_read_options), service_client(std::move(client)), is_valid(true) {
}

Azure::Storage::Blobs::BlobContainerClient
AzureBlobContextState::GetBlobContainerClient(const std::string &blobContainerName) const {
	return service_client.GetBlobContainerClient(blobContainerName);
}

bool AzureBlobContextState::IsValid() const {
	return is_valid;
}

void AzureBlobContextState::QueryEnd() {
	is_valid = false;
}

//////// AzureBlobStorageFileHandle ////////
AzureBlobStorageFileHandle::AzureBlobStorageFileHandle(FileSystem &fs, string path_p, uint8_t flags,
                                                       Azure::Storage::Blobs::BlobClient blob_client,
                                                       const AzureReadOptions &read_options)
    : FileHandle(fs, std::move(path_p)), flags(flags), length(0), last_modified(time_t()), buffer_available(0),
      buffer_idx(0), file_offset(0), buffer_start(0), buffer_end(0), blob_client(std::move(blob_client)),
      read_options(read_options) {
	try {
		auto res = blob_client.GetProperties();
		length = res.Value.BlobSize;
	} catch (Azure::Storage::StorageException &e) {
		throw IOException("AzureBlobStorageFileSystem open file '" + path + "' failed with code'" + e.ErrorCode +
		                  "',Reason Phrase: '" + e.ReasonPhrase + "', Message: '" + e.Message + "'");
	} catch (std::exception &e) {
		throw IOException(
		    "AzureBlobStorageFileSystem could not open file: '%s', unknown error occurred, this could mean "
		    "the credentials used were wrong. Original error message: '%s' ",
		    path, e.what());
	}

	if (flags & FileFlags::FILE_FLAGS_READ) {
		read_buffer = duckdb::unique_ptr<data_t[]>(new data_t[read_options.buffer_size]);
	}
}

//////// AzureBlobStorageFileSystem ////////
unique_ptr<AzureBlobStorageFileHandle> AzureBlobStorageFileSystem::CreateHandle(const string &path, uint8_t flags,
                                                                                FileLockType lock,
                                                                                FileCompressionType compression,
                                                                                FileOpener *opener) {
	if (opener == nullptr) {
		throw InternalException("Cannot do Azure storage CreateHandle without FileOpener");
	}

	D_ASSERT(compression == FileCompressionType::UNCOMPRESSED);

	auto parsed_url = ParseUrl(path);
	auto storage_context = GetOrCreateStorageContext(opener, path, parsed_url);
	auto container = storage_context->GetBlobContainerClient(parsed_url.container);
	auto blob_client = container.GetBlockBlobClient(parsed_url.path);

	return make_uniq<AzureBlobStorageFileHandle>(*this, path, flags, blob_client, storage_context->read_options);
}

unique_ptr<FileHandle> AzureBlobStorageFileSystem::OpenFile(const string &path, uint8_t flags, FileLockType lock,
                                                            FileCompressionType compression, FileOpener *opener) {
	D_ASSERT(compression == FileCompressionType::UNCOMPRESSED);

	if (flags & FileFlags::FILE_FLAGS_WRITE) {
		throw NotImplementedException("Writing to Azure containers is currently not supported");
	}

	auto handle = CreateHandle(path, flags, lock, compression, opener);
	return std::move(handle);
}

int64_t AzureBlobStorageFileSystem::GetFileSize(FileHandle &handle) {
	auto &afh = handle.Cast<AzureBlobStorageFileHandle>();
	return afh.length;
}

time_t AzureBlobStorageFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &afh = handle.Cast<AzureBlobStorageFileHandle>();
	return afh.last_modified;
}

bool AzureBlobStorageFileSystem::CanHandleFile(const string &fpath) {
	return fpath.rfind("azure://", 0) * fpath.rfind("az://", 0) == 0;
}

void AzureBlobStorageFileSystem::Seek(FileHandle &handle, idx_t location) {
	auto &sfh = handle.Cast<AzureBlobStorageFileHandle>();
	sfh.file_offset = location;
}

void AzureBlobStorageFileSystem::FileSync(FileHandle &handle) {
	throw NotImplementedException("FileSync for Azure Storage files not implemented");
}

int64_t AzureBlobStorageFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &hfh = handle.Cast<AzureBlobStorageFileHandle>();
	idx_t max_read = hfh.length - hfh.file_offset;
	nr_bytes = MinValue<idx_t>(max_read, nr_bytes);
	Read(handle, buffer, nr_bytes, hfh.file_offset);
	return nr_bytes;
}

vector<string> AzureBlobStorageFileSystem::Glob(const string &path, FileOpener *opener) {
	if (opener == nullptr) {
		throw InternalException("Cannot do Azure storage Glob without FileOpener");
	}

	auto azure_url = ParseUrl(path);
	auto storage_context = GetOrCreateStorageContext(opener, path, azure_url);

	// Azure matches on prefix, not glob pattern, so we take a substring until the first wildcard
	auto first_wildcard_pos = azure_url.path.find_first_of("*[\\");
	if (first_wildcard_pos == string::npos) {
		return {path};
	}

	string shared_path = azure_url.path.substr(0, first_wildcard_pos);
	auto container_client = storage_context->GetBlobContainerClient(azure_url.container);

	const auto pattern_splits = StringUtil::Split(azure_url.path, "/");
	vector<string> result;

	Azure::Storage::Blobs::ListBlobsOptions options;
	options.Prefix = shared_path;

	const auto path_result_prefix =
	    (azure_url.is_fully_qualified ? (azure_url.prefix + azure_url.storage_account_name + '.' + azure_url.endpoint +
	                                     '/' + azure_url.container)
	                                  : (azure_url.prefix + azure_url.container));
	while (true) {
		// Perform query
		Azure::Storage::Blobs::ListBlobsPagedResponse res;
		try {
			res = container_client.ListBlobs(options);
		} catch (Azure::Storage::StorageException &e) {
			throw IOException("AzureStorageFileSystem Read to %s failed with %s Reason Phrase: %s", path, e.ErrorCode,
			                  e.ReasonPhrase);
		}

		// Assuming that in the majority of the case it's wildcard
		result.reserve(result.size() + res.Blobs.size());

		// Ensure that the retrieved element match the expected pattern
		for (const auto &key : res.Blobs) {
			vector<string> key_splits = StringUtil::Split(key.Name, "/");
			bool is_match = Match(key_splits.begin(), key_splits.end(), pattern_splits.begin(), pattern_splits.end());

			if (is_match) {
				auto result_full_url = path_result_prefix + '/' + key.Name;
				result.push_back(result_full_url);
			}
		}

		// Manage Azure pagination
		if (res.NextPageToken) {
			options.ContinuationToken = res.NextPageToken;
		} else {
			break;
		}
	}

	return result;
}

// TODO: this code is identical to HTTPFS, look into unifying it
void AzureBlobStorageFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &hfh = handle.Cast<AzureBlobStorageFileHandle>();

	idx_t to_read = nr_bytes;
	idx_t buffer_offset = 0;

	// Don't buffer when DirectIO is set.
	if (hfh.flags & FileFlags::FILE_FLAGS_DIRECT_IO && to_read > 0) {
		ReadRange(hfh, location, (char *)buffer, to_read);
		hfh.buffer_available = 0;
		hfh.buffer_idx = 0;
		hfh.file_offset = location + nr_bytes;
		return;
	}

	if (location >= hfh.buffer_start && location < hfh.buffer_end) {
		hfh.file_offset = location;
		hfh.buffer_idx = location - hfh.buffer_start;
		hfh.buffer_available = (hfh.buffer_end - hfh.buffer_start) - hfh.buffer_idx;
	} else {
		// reset buffer
		hfh.buffer_available = 0;
		hfh.buffer_idx = 0;
		hfh.file_offset = location;
	}
	while (to_read > 0) {
		auto buffer_read_len = MinValue<idx_t>(hfh.buffer_available, to_read);
		if (buffer_read_len > 0) {
			D_ASSERT(hfh.buffer_start + hfh.buffer_idx + buffer_read_len <= hfh.buffer_end);
			memcpy((char *)buffer + buffer_offset, hfh.read_buffer.get() + hfh.buffer_idx, buffer_read_len);

			buffer_offset += buffer_read_len;
			to_read -= buffer_read_len;

			hfh.buffer_idx += buffer_read_len;
			hfh.buffer_available -= buffer_read_len;
			hfh.file_offset += buffer_read_len;
		}

		if (to_read > 0 && hfh.buffer_available == 0) {
			auto new_buffer_available = MinValue<idx_t>(hfh.read_options.buffer_size, hfh.length - hfh.file_offset);

			// Bypass buffer if we read more than buffer size
			if (to_read > new_buffer_available) {
				ReadRange(hfh, location + buffer_offset, (char *)buffer + buffer_offset, to_read);
				hfh.buffer_available = 0;
				hfh.buffer_idx = 0;
				hfh.file_offset += to_read;
				break;
			} else {
				ReadRange(hfh, hfh.file_offset, (char *)hfh.read_buffer.get(), new_buffer_available);
				hfh.buffer_available = new_buffer_available;
				hfh.buffer_idx = 0;
				hfh.buffer_start = hfh.file_offset;
				hfh.buffer_end = hfh.buffer_start + new_buffer_available;
			}
		}
	}
}

bool AzureBlobStorageFileSystem::FileExists(const string &filename) {
	try {
		auto handle = OpenFile(filename, FileFlags::FILE_FLAGS_READ);
		auto &sfh = handle->Cast<AzureBlobStorageFileHandle>();
		if (sfh.length == 0) {
			return false;
		}
		return true;
	} catch (...) {
		return false;
	};
}

void AzureBlobStorageFileSystem::ReadRange(FileHandle &handle, idx_t file_offset, char *buffer_out,
                                           idx_t buffer_out_len) {
	auto &afh = handle.Cast<AzureBlobStorageFileHandle>();

	try {
		// Specify the range
		Azure::Core::Http::HttpRange range;
		range.Offset = (int64_t)file_offset;
		range.Length = buffer_out_len;
		Azure::Storage::Blobs::DownloadBlobToOptions options;
		options.Range = range;
		options.TransferOptions.Concurrency = afh.read_options.transfer_concurrency;
		options.TransferOptions.InitialChunkSize = afh.read_options.transfer_chunk_size;
		options.TransferOptions.ChunkSize = afh.read_options.transfer_chunk_size;
		auto res = afh.blob_client.DownloadTo((uint8_t *)buffer_out, buffer_out_len, options);

	} catch (Azure::Storage::StorageException &e) {
		throw IOException("AzureBlobStorageFileSystem Read to " + afh.path + " failed with " + e.ErrorCode +
		                  "Reason Phrase: " + e.ReasonPhrase);
	}
}

std::shared_ptr<AzureBlobContextState>
AzureBlobStorageFileSystem::GetOrCreateStorageContext(FileOpener *opener, const std::string &path,
                                                      const AzureParsedUrl &parsed_url) {
	Value value;
	bool azure_context_caching = true;
	if (FileOpener::TryGetCurrentSetting(opener, "azure_context_caching", value)) {
		azure_context_caching = value.GetValue<bool>();
	}

	std::shared_ptr<AzureBlobContextState> result;
	if (azure_context_caching) {
		auto *client_context = FileOpener::TryGetClientContext(opener);

		auto &registered_state = client_context->registered_state;
		auto storage_account_it = registered_state.find(parsed_url.storage_account_name);
		if (storage_account_it == registered_state.end()) {
			result = CreateStorageContext(opener, path, parsed_url);
			registered_state.insert(std::make_pair(parsed_url.storage_account_name, result));
		} else {
			auto *azure_context_state = static_cast<AzureBlobContextState *>(storage_account_it->second.get());
			// We keep the context valid until the QueryEnd (cf: AzureBlobContextState#QueryEnd())
			// we do so because between queries the user can change the secret/variable that has been set
			// the side effect of that is that we will reconnect (potentially retrieve a new token) on each request
			if (!azure_context_state->IsValid()) {
				result = CreateStorageContext(opener, path, parsed_url);
				registered_state[parsed_url.storage_account_name] = result;
			} else {
				result = std::shared_ptr<AzureBlobContextState>(storage_account_it->second, azure_context_state);
			}
		}
	} else {
		result = CreateStorageContext(opener, path, parsed_url);
	}

	return result;
}

std::shared_ptr<AzureBlobContextState>
AzureBlobStorageFileSystem::CreateStorageContext(FileOpener *opener, const string &path,
                                                 const AzureParsedUrl &parsed_url) {
	auto azure_read_options = ParseAzureReadOptions(opener);

	return std::make_shared<AzureBlobContextState>(ConnectToBlobStorageAccount(opener, path, parsed_url),
	                                               azure_read_options);
}

} // namespace duckdb