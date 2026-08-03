#pragma once

#include "cldmux/detail/provider.hpp"

namespace cldmux {
namespace detail {

// AWS and Azure object storage ------------------------------------------------

inline std::string aws_storage_region(const client_state& client) {
    return region(configured_region(client.config, "aws"), "aws");
}

inline std::string aws_storage_url(const client_state& client, std::string_view bucket,
                                   std::string_view key = {}) {
    if (bucket.find('/') != std::string_view::npos)
        throw error("AWS bucket name must not contain '/'");
    const std::string selected_region = aws_storage_region(client);
    const std::string endpoint =
        aws_endpoint(client, client.config.aws.s3_endpoint, "s3", selected_region);
    std::string result = endpoint + '/' + gcp::detail::encode(bucket);
    if (!key.empty())
        result += '/' + encode_path(key);
    return result;
}

inline gcp::detail::HttpResponse aws_storage_call(const client_state& client,
                                                  gcp::detail::HttpRequest request,
                                                  bool retry = true) {
    return aws_call(client, std::move(request), aws_storage_region(client), "s3", retry);
}

inline std::string azure_storage_endpoint(const client_state& client) {
    std::string endpoint = gcp::detail::base_url(client.config.azure.storage_endpoint);
    if (endpoint.empty() && !client.config.azure.storage_account.empty())
        endpoint = "https://" + client.config.azure.storage_account + ".blob.core.windows.net";
    if (endpoint.empty())
        throw error("Azure object storage requires config::azure.storage_account or "
                    "storage_endpoint");
    validate_endpoint(client, endpoint, "Azure storage_endpoint");
    return endpoint;
}

inline std::string azure_storage_url(const client_state& client, std::string_view container,
                                     std::string_view key = {}) {
    std::string result = azure_storage_endpoint(client) + '/' + gcp::detail::encode(container);
    if (!key.empty())
        result += '/' + encode_path(key);
    return result;
}

inline object aws_object_from_headers(std::string name,
                                      const gcp::detail::HttpResponse& response) {
    object result;
    result.name = std::move(name);
    result.etag = response_header(response, "etag");
    // ETag is the portable AWS compare-and-replace token. It is deliberately
    // not labelled as MD5 because multipart and encrypted objects differ.
    result.generation = result.etag;
    const std::string size = response_header(response, "content-length");
    if (!size.empty())
        result.size = unsigned_text(size, "object size");
    result.content_type = response_header(response, "content-type");
    result.updated = response_header(response, "last-modified");
    result.crc32c = response_header(response, "x-amz-meta-cloud-crc32c");
    return result;
}

inline object azure_object_from_headers(std::string name,
                                        const gcp::detail::HttpResponse& response) {
    object result;
    result.name = std::move(name);
    result.etag = response_header(response, "etag");
    result.generation = result.etag;
    const std::string size = response_header(response, "content-length");
    if (!size.empty())
        result.size = unsigned_text(size, "blob size");
    result.content_type = response_header(response, "content-type");
    result.updated = response_header(response, "last-modified");
    result.crc32c = response_header(response, "x-ms-meta-cloudcrc32c");
    result.md5_hash = response_header(response, "content-md5");
    return result;
}

inline object aws_storage_stat(const client_state& client, const uri& location) {
    auto response = aws_storage_call(
        client, gcp::detail::HttpRequest{}
                    .with_method("HEAD")
                    .with_url(aws_storage_url(client, location.bucket, location.key))
                    .with_accept_json(false));
    return aws_object_from_headers(location.key, response);
}

inline object azure_storage_stat(const client_state& client, const uri& location) {
    auto response = azure_storage_call(
        client, gcp::detail::HttpRequest{}
                    .with_method("HEAD")
                    .with_url(azure_storage_url(client, location.bucket, location.key))
                    .with_accept_json(false));
    return azure_object_from_headers(location.key, response);
}

inline object aws_storage_put(const client_state& client, const uri& location,
                              std::string_view bytes, put_options options) {
    const std::string checksum = options.crc32c ? gcp::detail::crc32c(bytes) : std::string{};
    auto headers = conditional_headers(options);
    headers.push_back(gcp::detail::header("Content-Type", options.content_type));
    if (!checksum.empty()) {
        headers.push_back(gcp::detail::header("x-amz-checksum-crc32c", checksum));
        headers.push_back(gcp::detail::header("x-amz-meta-cloud-crc32c", checksum));
    }
    (void)aws_storage_call(client, gcp::detail::HttpRequest{}
                                      .with_method("PUT")
                                      .with_url(aws_storage_url(client, location.bucket,
                                                                location.key))
                                      .with_headers(std::move(headers))
                                      .with_body(std::string(bytes))
                                      .with_accept_json(false));
    const object result = aws_storage_stat(client, location);
    if (!checksum.empty() && result.crc32c != checksum)
        throw error("Amazon S3 upload checksum metadata mismatch");
    return result;
}

inline object azure_storage_put(const client_state& client, const uri& location,
                                std::string_view bytes, put_options options) {
    const std::string checksum = options.crc32c ? gcp::detail::crc32c(bytes) : std::string{};
    auto headers = conditional_headers(options);
    headers.push_back("x-ms-blob-type: BlockBlob");
    headers.push_back(gcp::detail::header("Content-Type", options.content_type));
    if (!checksum.empty())
        headers.push_back(gcp::detail::header("x-ms-meta-cloudcrc32c", checksum));
    (void)azure_storage_call(client, gcp::detail::HttpRequest{}
                                        .with_method("PUT")
                                        .with_url(azure_storage_url(client, location.bucket,
                                                                    location.key))
                                        .with_headers(std::move(headers))
                                        .with_body(std::string(bytes))
                                        .with_accept_json(false));
    const object result = azure_storage_stat(client, location);
    if (!checksum.empty() && result.crc32c != checksum)
        throw error("Azure Blob upload checksum metadata mismatch");
    return result;
}

inline object aws_storage_put_file(const client_state& client, const uri& location,
                                   const std::filesystem::path& source, put_options options) {
    const auto upload = gcp::detail::prepare_upload(source, options.crc32c);
    auto headers = conditional_headers(options);
    headers.push_back(gcp::detail::header("Content-Type", options.content_type));
    if (!upload->crc32c.empty()) {
        headers.push_back(gcp::detail::header("x-amz-checksum-crc32c", upload->crc32c));
        headers.push_back(gcp::detail::header("x-amz-meta-cloud-crc32c", upload->crc32c));
    }
    (void)aws_storage_call(client, gcp::detail::HttpRequest{}
                                      .with_method("PUT")
                                      .with_url(aws_storage_url(client, location.bucket,
                                                                location.key))
                                      .with_headers(std::move(headers))
                                      .with_upload_file(upload)
                                      .with_timeout(client.config.transfer_timeout)
                                      .with_accept_json(false));
    const object result = aws_storage_stat(client, location);
    if (!upload->crc32c.empty() && result.crc32c != upload->crc32c)
        throw error("Amazon S3 file upload checksum metadata mismatch");
    return result;
}

inline object azure_storage_put_file(const client_state& client, const uri& location,
                                     const std::filesystem::path& source, put_options options) {
    const auto upload = gcp::detail::prepare_upload(source, options.crc32c);
    auto headers = conditional_headers(options);
    headers.push_back("x-ms-blob-type: BlockBlob");
    headers.push_back(gcp::detail::header("Content-Type", options.content_type));
    if (!upload->crc32c.empty())
        headers.push_back(gcp::detail::header("x-ms-meta-cloudcrc32c", upload->crc32c));
    (void)azure_storage_call(client, gcp::detail::HttpRequest{}
                                        .with_method("PUT")
                                        .with_url(azure_storage_url(client, location.bucket,
                                                                    location.key))
                                        .with_headers(std::move(headers))
                                        .with_upload_file(upload)
                                        .with_timeout(client.config.transfer_timeout)
                                        .with_accept_json(false));
    const object result = azure_storage_stat(client, location);
    if (!upload->crc32c.empty() && result.crc32c != upload->crc32c)
        throw error("Azure Blob file upload checksum metadata mismatch");
    return result;
}

inline std::string aws_storage_get(const client_state& client, const uri& location) {
    const object metadata = aws_storage_stat(client, location);
    std::vector<std::string> headers;
    if (!metadata.generation.empty())
        headers.push_back(gcp::detail::header("If-Match", metadata.generation));
    auto response = aws_storage_call(
        client, gcp::detail::HttpRequest{}
                    .with_url(aws_storage_url(client, location.bucket, location.key))
                    .with_headers(std::move(headers))
                    .with_calculate_crc32c(!metadata.crc32c.empty())
                    .with_accept_json(false));
    if (!metadata.crc32c.empty() && response.crc32c != metadata.crc32c)
        throw error("Amazon S3 download checksum mismatch");
    return response.body;
}

inline std::string azure_storage_get(const client_state& client, const uri& location) {
    const object metadata = azure_storage_stat(client, location);
    std::vector<std::string> headers;
    if (!metadata.generation.empty())
        headers.push_back(gcp::detail::header("If-Match", metadata.generation));
    auto response = azure_storage_call(
        client, gcp::detail::HttpRequest{}
                    .with_url(azure_storage_url(client, location.bucket, location.key))
                    .with_headers(std::move(headers))
                    .with_calculate_crc32c(!metadata.crc32c.empty())
                    .with_accept_json(false));
    if (!metadata.crc32c.empty() && response.crc32c != metadata.crc32c)
        throw error("Azure Blob download checksum mismatch");
    return response.body;
}

inline void aws_storage_get_file(const client_state& client, const uri& location,
                                 const std::filesystem::path& destination) {
    const object metadata = aws_storage_stat(client, location);
    std::vector<std::string> headers;
    if (!metadata.generation.empty())
        headers.push_back(gcp::detail::header("If-Match", metadata.generation));
    auto request = gcp::detail::HttpRequest{}
                       .with_url(aws_storage_url(client, location.bucket, location.key))
                       .with_headers(std::move(headers))
                       .with_accept_json(false);
    request.download_file = destination;
    request.timeout = client.config.transfer_timeout;
    if (!metadata.crc32c.empty())
        request.expected_crc32c = metadata.crc32c;
    (void)aws_storage_call(client, std::move(request));
}

inline void azure_storage_get_file(const client_state& client, const uri& location,
                                   const std::filesystem::path& destination) {
    const object metadata = azure_storage_stat(client, location);
    std::vector<std::string> headers;
    if (!metadata.generation.empty())
        headers.push_back(gcp::detail::header("If-Match", metadata.generation));
    auto request = gcp::detail::HttpRequest{}
                       .with_url(azure_storage_url(client, location.bucket, location.key))
                       .with_headers(std::move(headers))
                       .with_accept_json(false);
    request.download_file = destination;
    request.timeout = client.config.transfer_timeout;
    if (!metadata.crc32c.empty())
        request.expected_crc32c = metadata.crc32c;
    (void)azure_storage_call(client, std::move(request));
}

inline void aws_storage_remove(const client_state& client, const uri& location) {
    (void)aws_storage_call(client, gcp::detail::HttpRequest{}
                                      .with_method("DELETE")
                                      .with_url(aws_storage_url(client, location.bucket,
                                                                location.key))
                                      .with_accept_json(false));
}

inline void azure_storage_remove(const client_state& client, const uri& location) {
    (void)azure_storage_call(client, gcp::detail::HttpRequest{}
                                        .with_method("DELETE")
                                        .with_url(azure_storage_url(client, location.bucket,
                                                                    location.key))
                                        .with_accept_json(false));
}

inline object_list aws_storage_list(const client_state& client, const uri& location,
                                    list_options options) {
    if (options.versions)
        throw error("AWS object version listing is not implemented by the portable facade");
    object_list result;
    std::string token;
    for (int page = 0; page < 1000; ++page) {
        std::size_t page_size = 1000;
        if (options.limit)
            page_size = std::min(page_size, options.limit - result.objects.size());
        if (page_size == 0)
            return result;
        std::string query = "?list-type=2&encoding-type=url&max-keys=" +
                            std::to_string(page_size);
        if (!options.prefix.empty())
            query += "&prefix=" + gcp::detail::encode(options.prefix);
        if (!options.delimiter.empty())
            query += "&delimiter=" + gcp::detail::encode(options.delimiter);
        if (!token.empty())
            query += "&continuation-token=" + gcp::detail::encode(token);
        const auto response = aws_storage_call(
            client, gcp::detail::HttpRequest{}
                        .with_url(aws_storage_url(client, location.bucket) + query)
                        .with_accept_json(false));
        for (const auto block : xml_blocks(response.body, "Contents")) {
            object item;
            item.name = percent_decode(xml_field(block, "Key"));
            item.generation = xml_field(block, "ETag");
            item.etag = item.generation;
            const std::string size = xml_field(block, "Size");
            if (!size.empty())
                item.size = unsigned_text(size, "S3 object size");
            item.updated = xml_field(block, "LastModified");
            result.objects.push_back(std::move(item));
            if (options.limit && result.objects.size() == options.limit)
                return result;
        }
        for (const auto block : xml_blocks(response.body, "CommonPrefixes"))
            result.prefixes.push_back(percent_decode(xml_field(block, "Prefix")));
        const bool truncated = xml_field(response.body, "IsTruncated") == "true";
        const std::string next = xml_field(response.body, "NextContinuationToken");
        if (!truncated)
            return result;
        if (next.empty() || next == token)
            throw error("AWS S3 listing pagination did not advance");
        token = next;
    }
    throw error("AWS S3 listing exceeded 1000 pages");
}

inline object_list azure_storage_list(const client_state& client, const uri& location,
                                      list_options options) {
    if (options.versions)
        throw error("Azure Blob version listing is not implemented by the portable facade");
    object_list result;
    std::string marker;
    for (int page = 0; page < 1000; ++page) {
        std::size_t page_size = 5000;
        if (options.limit)
            page_size = std::min(page_size, options.limit - result.objects.size());
        if (page_size == 0)
            return result;
        std::string query = "?restype=container&comp=list&include=metadata&maxresults=" +
                            std::to_string(page_size);
        if (!options.prefix.empty())
            query += "&prefix=" + gcp::detail::encode(options.prefix);
        if (!options.delimiter.empty())
            query += "&delimiter=" + gcp::detail::encode(options.delimiter);
        if (!marker.empty())
            query += "&marker=" + gcp::detail::encode(marker);
        const auto response = azure_storage_call(
            client, gcp::detail::HttpRequest{}
                        .with_url(azure_storage_url(client, location.bucket) + query)
                        .with_accept_json(false));
        for (const auto block : xml_blocks(response.body, "Blob")) {
            object item;
            item.name = azure_xml_name(block);
            const std::string properties = xml_raw_field(block, "Properties");
            item.generation = xml_field(properties, "Etag");
            item.etag = item.generation;
            const std::string size = xml_field(properties, "Content-Length");
            if (!size.empty())
                item.size = unsigned_text(size, "Azure Blob size");
            item.content_type = xml_field(properties, "Content-Type");
            item.updated = xml_field(properties, "Last-Modified");
            item.md5_hash = xml_field(properties, "Content-MD5");
            const std::string metadata = xml_raw_field(block, "Metadata");
            item.crc32c = xml_field(metadata, "cloudcrc32c");
            result.objects.push_back(std::move(item));
            if (options.limit && result.objects.size() == options.limit)
                return result;
        }
        for (const auto block : xml_blocks(response.body, "BlobPrefix"))
            result.prefixes.push_back(azure_xml_name(block));
        const std::string next = xml_field(response.body, "NextMarker");
        if (next.empty())
            return result;
        if (next == marker)
            throw error("Azure Blob listing pagination did not advance");
        marker = next;
    }
    throw error("Azure Blob listing exceeded 1000 pages");
}

} // namespace detail
} // namespace cldmux
