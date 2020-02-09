// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <string>
#include <vector>

#include <fmt/format.h>
#include <glad/glad.h>

#include "common/common_types.h"
#include "video_core/renderer_opengl/utils.h"

namespace OpenGL {

struct VertexArrayPushBuffer::Entry {
    GLuint binding_index{};
    const GLuint* buffer{};
    GLintptr offset{};
    GLsizei stride{};
};

VertexArrayPushBuffer::VertexArrayPushBuffer() = default;

VertexArrayPushBuffer::~VertexArrayPushBuffer() = default;

void VertexArrayPushBuffer::Setup(GLuint vao_) {
    vao = vao_;
    index_buffer = nullptr;
    vertex_buffers.clear();
}

void VertexArrayPushBuffer::SetIndexBuffer(const GLuint* buffer) {
    index_buffer = buffer;
}

void VertexArrayPushBuffer::SetVertexBuffer(GLuint binding_index, const GLuint* buffer,
                                            GLintptr offset, GLsizei stride) {
    vertex_buffers.push_back(Entry{binding_index, buffer, offset, stride});
}

void VertexArrayPushBuffer::Bind() {
    if (index_buffer) {
        glVertexArrayElementBuffer(vao, *index_buffer);
    }

    // TODO(Rodrigo): Find a way to ARB_multi_bind this
    for (const auto& entry : vertex_buffers) {
        glVertexArrayVertexBuffer(vao, entry.binding_index, *entry.buffer, entry.offset,
                                  entry.stride);
    }
}

struct BindBuffersRangePushBuffer::Entry {
    GLuint binding;
    const GLuint* buffer;
    GLintptr offset;
    GLsizeiptr size;
};

BindBuffersRangePushBuffer::BindBuffersRangePushBuffer(GLenum target) : target{target} {}

BindBuffersRangePushBuffer::~BindBuffersRangePushBuffer() = default;

void BindBuffersRangePushBuffer::Setup() {
    entries.clear();
}

void BindBuffersRangePushBuffer::Push(GLuint binding, const GLuint* buffer, GLintptr offset,
                                      GLsizeiptr size) {
    entries.push_back(Entry{binding, buffer, offset, size});
}

void BindBuffersRangePushBuffer::Bind() {
    for (const Entry& entry : entries) {
        glBindBufferRange(target, entry.binding, *entry.buffer, entry.offset, entry.size);
    }
}

void LabelGLObject(GLenum identifier, GLuint handle, VAddr addr, std::string_view extra_info) {
    if (!GLAD_GL_KHR_debug) {
        // We don't need to throw an error as this is just for debugging
        return;
    }

    std::string object_label;
    if (extra_info.empty()) {
        switch (identifier) {
        case GL_TEXTURE:
            object_label = fmt::format("Texture@0x{:016X}", addr);
            break;
        case GL_PROGRAM:
            object_label = fmt::format("Shader@0x{:016X}", addr);
            break;
        default:
            object_label = fmt::format("Object(0x{:X})@0x{:016X}", identifier, addr);
            break;
        }
    } else {
        object_label = fmt::format("{}@0x{:016X}", extra_info, addr);
    }
    glObjectLabel(identifier, handle, -1, static_cast<const GLchar*>(object_label.c_str()));
}

} // namespace OpenGL
