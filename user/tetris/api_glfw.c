#define GLEW_STATIC
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "api.h"

#define FPS     60
#define TEX_W   400
#define TEX_H   240

#define WIN_W   (TEX_W * 2)
#define WIN_H   (TEX_H * 2)

static GLFWwindow *window; 
static bool buttons_updated = false;
static uint32_t last_buttons;

static float vertices[6][4] = {
    {-1, -1, 0, 0},
    {1, -1, 1, 0},
    {1, 1, 1, -1},
    {-1, -1, 0, 0},
    {1, 1, 1, -1},
    {-1, 1, 0, -1},
};

static uint8_t buf[TEX_W * TEX_H * 3];

static void glfw_err_callback(int error, const char *desc)
{
    fprintf(stderr, "> <  GLFW: (%d) %s\n", error, desc);
}

static void glfw_fbsz_callback(GLFWwindow *window, int w, int h)
{
    glViewport(0, 0, w, h);

    float xs = (float)w / TEX_W;
    float ys = (float)h / TEX_H;
    float s = (xs < ys ? xs : ys);
    float x = s / xs;
    float y = s / ys;
    for (int i = 0; i < 6; i++) {
        vertices[i][0] = x;
        vertices[i][1] = y;
    }
#define FLIP(_a, _b) (vertices[_a][_b] = -vertices[_a][_b])
    FLIP(0, 0); FLIP(0, 1); FLIP(1, 1);
    FLIP(3, 0); FLIP(3, 1); FLIP(5, 0);
#undef FLIP
    glBufferData(GL_ARRAY_BUFFER,
        24 * sizeof(float), vertices, GL_STREAM_DRAW);
}

static inline GLuint load_shader(GLenum type, const char *source)
{
    GLuint shader_id = glCreateShader(type);
    glShaderSource(shader_id, 1, &source, NULL);
    glCompileShader(shader_id);

    GLint status;
    glGetShaderiv(shader_id, GL_COMPILE_STATUS, &status);
    char msg_buf[1024];
    glGetShaderInfoLog(shader_id, sizeof(msg_buf) - 1, NULL, msg_buf);
    fprintf(stderr, "OvO  Compilation log for %s shader\n",
        (type == GL_VERTEX_SHADER ? "vertex" :
         type == GL_FRAGMENT_SHADER ? "fragment" : "unknown (!)"));
    fputs(msg_buf, stderr);
    fprintf(stderr, "=v=  End\n");
    if (status != GL_TRUE) {
        fprintf(stderr, "> <  Shader compilation failed\n");
        return 0;
    }

    return shader_id;
}

int main()
{
    // -- Initialization --

    glfwSetErrorCallback(glfw_err_callback);

    if (!glfwInit()) {
        fprintf(stderr, "> <  Cannot initialize GLFW\n");
        return 2;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);

    window = glfwCreateWindow(WIN_W, WIN_H, "MIKAN", NULL, NULL);
    if (window == NULL) {
        fprintf(stderr, "> <  Cannot create GLFW window\n");
        return 2;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "> <  Cannot initialize GLEW\n");
        return 2;
    }

    // -- OpenGL setup --

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

#define GLSL(__source) "#version 120\n" #__source

    const char *vshader_source = GLSL(
        attribute vec2 screen_pos;
        attribute vec2 texture_pos;
        varying vec2 texture_pos_;
        void main()
        {
            gl_Position = vec4(screen_pos, 0.0, 1.0);
            texture_pos_ = texture_pos;
        }
    );

    const char *fshader_source = GLSL(
        varying vec2 texture_pos_;
        uniform sampler2D tex;
        void main()
        {
            gl_FragColor = texture2D(tex, texture_pos_);
        }
    );

    GLuint vshader = load_shader(GL_VERTEX_SHADER, vshader_source);
    GLuint fshader = load_shader(GL_FRAGMENT_SHADER, fshader_source);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vshader);
    glAttachShader(prog, fshader);
    glLinkProgram(prog);
    glUseProgram(prog);

    GLuint screen_pos_attrib_index = glGetAttribLocation(prog, "screen_pos");
    glEnableVertexAttribArray(screen_pos_attrib_index);
    glVertexAttribPointer(screen_pos_attrib_index, 2, GL_FLOAT, GL_FALSE,
        4 * sizeof(float), 0);

    GLuint texture_pos_attrib_index = glGetAttribLocation(prog, "texture_pos");
    glEnableVertexAttribArray(texture_pos_attrib_index);
    glVertexAttribPointer(texture_pos_attrib_index, 2, GL_FLOAT, GL_FALSE,
        4 * sizeof(float), (const void *)(2 * sizeof(float)));

    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);

    glBufferData(GL_ARRAY_BUFFER,
        24 * sizeof(float), vertices, GL_STREAM_DRAW);

    // -- Event/render loop --

    init();

    float last_time = glfwGetTime(), cur_time;

    glfwSetFramebufferSizeCallback(window, glfw_fbsz_callback);

    while (!glfwWindowShouldClose(window)) {
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        glClearColor(0.7f, 0.7f, 0.7f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        update();

        while ((cur_time = glfwGetTime()) < last_time + 1.0f / FPS) usleep(1000);
        last_time = cur_time;

        // TODO: Optionally skip draw() calls
        void *nbuf = draw();
        memcpy(buf, nbuf, sizeof buf);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, TEX_W, TEX_H,
            0, GL_BGR, GL_UNSIGNED_BYTE, buf);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
        buttons_updated = false;
    }

    return 0;
}

void register_loop(update_func_t update, draw_func_t draw)
{
}

uint32_t buttons()
{
    if (buttons_updated) return last_buttons;

    uint32_t ret =
        (glfwGetKey(window, GLFW_KEY_UP) << 0) |
        (glfwGetKey(window, GLFW_KEY_DOWN) << 1) |
        (glfwGetKey(window, GLFW_KEY_LEFT) << 2) |
        (glfwGetKey(window, GLFW_KEY_RIGHT) << 3) |
        (glfwGetKey(window, GLFW_KEY_C) << 4) |
        (glfwGetKey(window, GLFW_KEY_X) << 5) |
        (glfwGetKey(window, GLFW_KEY_Z) << 6) |
        (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) << 7) |
        (glfwGetKey(window, GLFW_KEY_W) << 0) |     // Alternative set of keys
        (glfwGetKey(window, GLFW_KEY_S) << 1) |
        (glfwGetKey(window, GLFW_KEY_A) << 2) |
        (glfwGetKey(window, GLFW_KEY_D) << 3) |
        (glfwGetKey(window, GLFW_KEY_K) << 4) |
        (glfwGetKey(window, GLFW_KEY_L) << 5) |
        (glfwGetKey(window, GLFW_KEY_J) << 6) |
        (glfwGetKey(window, GLFW_KEY_I) << 7);
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1)) {
        GLFWgamepadstate state;
        if (glfwGetGamepadState(GLFW_JOYSTICK_1, &state)) ret |=
            (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] << 0) |
            (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] << 1) |
            (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] << 2) |
            (state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] << 3) |
            (state.buttons[GLFW_GAMEPAD_BUTTON_A] << 4) |
            (state.buttons[GLFW_GAMEPAD_BUTTON_B] << 5) |
            (state.buttons[GLFW_GAMEPAD_BUTTON_X] << 6) |
            (state.buttons[GLFW_GAMEPAD_BUTTON_Y] << 7);
    }
    buttons_updated = true;
    return (last_buttons = ret);
}
