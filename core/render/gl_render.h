#pragma once

// OpenGL ES / OpenGL YUV420P shader-based renderer.
// Shared across Android (EGL+GLES), iOS (EAGL+GLES), Desktop (SDL+GL).
//
// Usage:
//   GLVideoRender renderer;
//   renderer.init(width, height);   // call with GL context active
//   renderer.render(frame);         // upload YUV textures + draw quad
//   renderer.destroy();

#ifdef __APPLE__
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>
#elif defined(__ANDROID__)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
// Desktop: use SDL's OpenGL headers
#include <SDL2/SDL_opengl.h>
#endif

extern "C" {
#include <libavutil/frame.h>
}

#include "common/log.h"

namespace sp {

class GLVideoRender {
public:
    ~GLVideoRender() { destroy(); }

    bool init(int width, int height) {
        width_ = width;
        height_ = height;

        // Compile shaders
        program_ = create_program(vertex_shader_src, fragment_shader_src);
        if (!program_) {
            SP_LOGE("GLRender", "Failed to create shader program");
            return false;
        }

        // Get attribute/uniform locations
        attr_position_ = glGetAttribLocation(program_, "a_position");
        attr_texcoord_ = glGetAttribLocation(program_, "a_texCoord");
        uniform_y_ = glGetUniformLocation(program_, "u_textureY");
        uniform_u_ = glGetUniformLocation(program_, "u_textureU");
        uniform_v_ = glGetUniformLocation(program_, "u_textureV");

        // Create Y, U, V textures
        glGenTextures(3, textures_);
        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, textures_[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        // Initialize textures with correct sizes
        glBindTexture(GL_TEXTURE_2D, textures_[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, textures_[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width / 2, height / 2, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, textures_[2]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width / 2, height / 2, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

        initialized_ = true;
        SP_LOGI("GLRender", "Initialized %dx%d (GL)", width, height);
        return true;
    }

    void render(const AVFrame* frame) {
        if (!initialized_ || !frame) return;
        if (frame->format != 0 /* AV_PIX_FMT_YUV420P */) {
            SP_LOGW("GLRender", "Unexpected pixel format: %d, expected YUV420P", frame->format);
            return;
        }

        glUseProgram(program_);

        // Upload Y plane
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures_[0]);
        upload_plane(frame->data[0], frame->linesize[0], width_, height_);
        glUniform1i(uniform_y_, 0);

        // Upload U plane
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, textures_[1]);
        upload_plane(frame->data[1], frame->linesize[1], width_ / 2, height_ / 2);
        glUniform1i(uniform_u_, 1);

        // Upload V plane
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, textures_[2]);
        upload_plane(frame->data[2], frame->linesize[2], width_ / 2, height_ / 2);
        glUniform1i(uniform_v_, 2);

        // Draw fullscreen quad
        // Position: 2 floats, TexCoord: 2 floats, interleaved
        static const float vertices[] = {
            // x,    y,    u,   v
            -1.0f, -1.0f, 0.0f, 1.0f,  // bottom-left
             1.0f, -1.0f, 1.0f, 1.0f,  // bottom-right
            -1.0f,  1.0f, 0.0f, 0.0f,  // top-left
             1.0f,  1.0f, 1.0f, 0.0f,  // top-right
        };

        glVertexAttribPointer(attr_position_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
        glEnableVertexAttribArray(attr_position_);
        glVertexAttribPointer(attr_texcoord_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices + 2);
        glEnableVertexAttribArray(attr_texcoord_);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void set_viewport(int x, int y, int w, int h) {
        glViewport(x, y, w, h);
    }

    void destroy() {
        if (textures_[0]) {
            glDeleteTextures(3, textures_);
            textures_[0] = textures_[1] = textures_[2] = 0;
        }
        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        initialized_ = false;
    }

private:
    void upload_plane(const uint8_t* data, int linesize, int w, int h) {
        if (linesize == w) {
            // Tightly packed — single upload
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
        } else {
            // Stride mismatch — upload row by row
            for (int row = 0; row < h; row++) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, w, 1,
                                GL_LUMINANCE, GL_UNSIGNED_BYTE, data + row * linesize);
            }
        }
    }

    static GLuint compile_shader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[512];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            SP_LOGE("GLRender", "Shader compile error: %s", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    static GLuint create_program(const char* vs_src, const char* fs_src) {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
        if (!vs || !fs) return 0;

        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);

        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            SP_LOGE("GLRender", "Program link error: %s", log);
            glDeleteProgram(program);
            program = 0;
        }

        glDeleteShader(vs);
        glDeleteShader(fs);
        return program;
    }

    // --- Shaders ---
    static constexpr const char* vertex_shader_src =
        "attribute vec4 a_position;\n"
        "attribute vec2 a_texCoord;\n"
        "varying vec2 v_texCoord;\n"
        "void main() {\n"
        "    gl_Position = a_position;\n"
        "    v_texCoord = a_texCoord;\n"
        "}\n";

    // YUV→RGB conversion using BT.601 coefficients
    static constexpr const char* fragment_shader_src =
        #ifdef __APPLE__
        "precision mediump float;\n"
        #elif defined(__ANDROID__)
        "precision mediump float;\n"
        #else
        "" // Desktop GL doesn't need precision qualifiers
        #endif
        "varying vec2 v_texCoord;\n"
        "uniform sampler2D u_textureY;\n"
        "uniform sampler2D u_textureU;\n"
        "uniform sampler2D u_textureV;\n"
        "void main() {\n"
        "    float y = texture2D(u_textureY, v_texCoord).r;\n"
        "    float u = texture2D(u_textureU, v_texCoord).r - 0.5;\n"
        "    float v = texture2D(u_textureV, v_texCoord).r - 0.5;\n"
        "    float r = y + 1.402 * v;\n"
        "    float g = y - 0.344136 * u - 0.714136 * v;\n"
        "    float b = y + 1.772 * u;\n"
        "    gl_FragColor = vec4(r, g, b, 1.0);\n"
        "}\n";

    GLuint program_ = 0;
    GLuint textures_[3] = {0, 0, 0};
    GLint attr_position_ = -1;
    GLint attr_texcoord_ = -1;
    GLint uniform_y_ = -1;
    GLint uniform_u_ = -1;
    GLint uniform_v_ = -1;
    int width_ = 0, height_ = 0;
    bool initialized_ = false;
};

} // namespace sp
