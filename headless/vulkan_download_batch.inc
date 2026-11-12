    if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
        // Keep independent copies in one submission, then publish guest writes
        // in modification order. A staging allocation stays busy until its tick.
        const ::Eden::StagingCaller::Scope staging_caller{"TC_DownloadMemory"};
        using Map = decltype(runtime.DownloadStagingBuffer(size_t{}));
        std::vector<std::pair<ImageId, Map>> pending;
        pending.reserve(16);
        constexpr size_t batch_limit = 32_MiB;
        size_t batch_bytes = 0;
        const auto finish_batch = [&] {
            if (pending.empty()) return;
            runtime.Finish();
            for (auto& [image_id, map] : pending) {
                Image& image = slot_images[image_id];
                const auto copies = FullDownloadCopies(image.info);
                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span,
                             swizzle_data_buffer);
                if (SkipStaleUploads()) {
                    ClearStaleImage(image); // UB-128: the write-back has reached guest memory.
                }
            }
            pending.clear();
            batch_bytes = 0;
        };
        for (const ImageId image_id : images) {
            Image& image = slot_images[image_id];
            const size_t bytes = image.unswizzled_size_bytes;
            if (pending.size() == 16 || bytes > batch_limit - batch_bytes)
                finish_batch();
            auto staged = runtime.TryDownloadStagingBuffer(bytes);
            if (!staged && !pending.empty()) {
                // Finishing the batch frees its buffers, and the pool hands those out even in the
                // pause after a refusal.
                finish_batch();
                staged = runtime.TryDownloadStagingBuffer(bytes);
            }
            const bool kept_buffer = !staged;
            if (kept_buffer) {
                // No staging buffer: the pool's small buffer of its own, one image at a time. A
                // larger image stays marked and the next read asks again; this read sees what
                // guest memory holds.
                const auto kept = runtime.ReservedDownloadBuffer();
                if (bytes > kept.mapped_span.size()) {
                    static u64 times = 0;
                    if (++times; (times & (times - 1)) == 0) { // the 1st, 2nd, 4th, 8th ... time
                        LOG_WARNING(HW_GPU, "No staging buffer to download the image at {:#x}, {} "
                                            "times so far", image.gpu_addr, times);
                    }
                    image.flags |= ImageFlagBits::GpuModified;
                    FlushOnNextRead(image);
                    continue;
                }
                staged = kept;
            }
            auto map = *staged;
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            pending.emplace_back(image_id, std::move(map));
            // Oversized images retain the original one-image allocation/wait.
            if (bytes >= batch_limit || kept_buffer) {
                finish_batch();
            } else {
                batch_bytes += bytes;
            }
        }
        finish_batch();
        return;
    }
