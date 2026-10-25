#pragma once

#include <CesiumAsync/IAssetAccessor.h>

#include <mln/util/thread.hpp>

#include <memory>

namespace mln {
namespace cesium {

/// cesium-native's network access routed through MapLibre's own file source: the same
/// HTTP stack the map's tiles use, so on Android it is the OkHttp client the app
/// registered (its trust store, interceptors and SSO headers) and on desktop the
/// platform's HTTP file source. Requests run on a thread of their own, so loading goes
/// on while the map is still.
class FileSourceAssetAccessor final : public CesiumAsync::IAssetAccessor {
public:
    FileSourceAssetAccessor();
    ~FileSourceAssetAccessor() override;

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> get(
        const CesiumAsync::AsyncSystem& asyncSystem,
        const std::string& url,
        const std::vector<THeader>& headers) override;

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
        const CesiumAsync::AsyncSystem& asyncSystem,
        const std::string& verb,
        const std::string& url,
        const std::vector<THeader>& headers,
        const std::span<const std::byte>& contentPayload) override;

    void tick() noexcept override {}

    class Fetcher;

private:
    std::unique_ptr<util::Thread<Fetcher>> thread;
};

} // namespace cesium
} // namespace mln
