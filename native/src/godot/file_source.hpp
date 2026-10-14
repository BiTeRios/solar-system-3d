// Источник байтов для эфемерид через FileAccess Godot: работает и с файлами внутри .pck.
#pragma once

#include <godot_cpp/classes/file_access.hpp>

#include "core/ephemeris.hpp"

namespace godot {

class GodotFileSource : public sc::ByteSource {
public:
    explicit GodotFileSource(Ref<FileAccess> f) : f_(f) {}
    bool read(uint64_t offset, size_t size, void *dst) override {
        f_->seek(offset);
        return f_->get_buffer(static_cast<uint8_t *>(dst), size) == size;
    }

private:
    Ref<FileAccess> f_;
};

}  // namespace godot
