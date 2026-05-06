#include "keyboard_input.h"
#include "compat/input_keys.h"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#if LV_USE_SDL
#include "lvgl/src/drivers/sdl/lv_sdl_mouse.h"
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"
#include "lvgl/src/drivers/sdl/lv_sdl_keyboard.h"
#endif

#if LV_USE_EVDEV
#include <pthread.h>
#endif

namespace {

constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 170;
constexpr int kHeaderHeight = 24;
constexpr int kLineHeight = 15;

struct Job {
    std::string id;
    std::string name;
    std::string status;
    bool installed = false;
    std::string session;
    std::string updated;
    std::string app_dir;
    std::string log_file;
    std::string message;
};

struct ChatEntry {
    std::string time;
    std::string role;
    std::string text;
};

enum class Screen {
    Home,
    InputName,
    InputRequest,
    Detail,
    Log,
    Chat,
    ConfirmStart,
    FrozenWait,
    Modify,
    ExitPrompt,
    DeletePrompt,
    Terminal,
};

lv_obj_t *g_root = nullptr;
lv_indev_t *g_keyboard_indev = nullptr;
lv_group_t *g_group = nullptr;
lv_timer_t *g_refresh_timer = nullptr;
volatile sig_atomic_t g_quit_requested = 0;

std::string g_app_dir = ".";
std::string g_script_path = "vibapp.py";
std::string g_dep_status = "checking";
bool g_deps_ok = false;
std::vector<Job> g_jobs;
int g_selected = 0;
Screen g_screen = Screen::Home;
Screen g_terminal_return_screen = Screen::Detail;
std::string g_name_input;
std::string g_request_input;
std::string g_modify_input;
std::string g_status_message;
std::string g_pending_action;
std::string g_pending_job_id;
std::string g_pending_name;
std::string g_pending_request;
Screen g_confirm_cancel_screen = Screen::InputRequest;
std::string g_frozen_job_id;
std::vector<std::string> g_log_lines;
std::vector<ChatEntry> g_chat_entries;
int g_log_offset = 0;
int g_chat_offset = 0;
bool g_log_wrap = false;
bool g_chat_wrap = true;

constexpr int kTermRows = 16;
constexpr int kTermCols = 58;
int g_term_fd = -1;
pid_t g_term_pid = -1;
bool g_term_running = false;
std::string g_term_title;
std::vector<std::string> g_term_lines;
std::string g_term_escape;
int g_term_col = 0;
lv_timer_t *g_terminal_timer = nullptr;
uint32_t g_escape_pressed_tick = 0;
bool g_escape_long_fired = false;
constexpr uint32_t kEscapeLongPressMs = 750;

void render();
void terminal_poll();
void terminal_timer_cb(lv_timer_t *);
void select_job_by_id(const std::string &job_id);

const char *getenv_default(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return value ? value : fallback;
}

void request_quit()
{
    g_quit_requested = 1;
}

void handle_signal(int)
{
    request_quit();
}

std::string dirname_of(const char *argv0)
{
    if (!argv0 || !argv0[0]) return ".";
    char resolved[1024] = {};
    const char *path = argv0;
    if (realpath(argv0, resolved)) path = resolved;
    std::string value(path);
    size_t slash = value.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return value.substr(0, slash);
}

std::string home_state_dir()
{
    const char *env = std::getenv("VIBAPP_HOME");
    if (env && env[0]) return env;
    const char *home = std::getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    return std::string(home) + "/.local/share/vibapp";
}

std::string shell_quote(const std::string &value)
{
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
}

std::string run_capture(const std::string &cmd)
{
    std::string output;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return output;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    pclose(pipe);
    return output;
}

std::string tsv_unescape(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            char next = value[++i];
            if (next == 't') out += '\t';
            else if (next == 'n') out += '\n';
            else if (next == 'r') out += '\r';
            else out += next;
        } else {
            out += value[i];
        }
    }
    return out;
}

std::vector<std::string> split_tab(const std::string &line)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : line) {
        if (ch == '\t') {
            out.push_back(tsv_unescape(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(tsv_unescape(cur));
    return out;
}

std::string one_line(std::string value, size_t max_len)
{
    for (char &ch : value) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    if (value.size() > max_len) {
        value.resize(max_len > 3 ? max_len - 3 : max_len);
        value += "...";
    }
    return value;
}

std::string clean_inline(std::string value)
{
    for (char &ch : value) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    return value;
}

void push_wrapped(std::vector<std::string> &out, std::string value, size_t width, size_t max_rows = 18)
{
    value = clean_inline(value);
    if (width == 0) width = 1;
    if (value.empty()) {
        out.emplace_back();
        return;
    }
    size_t pushed = 0;
    size_t pos = 0;
    while (pos < value.size() && pushed < max_rows) {
        size_t len = std::min(width, value.size() - pos);
        if (len == width && pos + len < value.size()) {
            size_t break_pos = value.rfind(' ', pos + len);
            if (break_pos != std::string::npos && break_pos > pos + width / 2) {
                len = break_pos - pos;
            }
        }
        std::string part = value.substr(pos, len);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        out.push_back(part);
        ++pushed;
        pos += len;
        while (pos < value.size() && value[pos] == ' ') ++pos;
    }
}

void terminal_clear()
{
    g_term_lines.clear();
    g_term_lines.emplace_back();
    g_term_escape.clear();
    g_term_col = 0;
}

void terminal_newline()
{
    if (g_term_lines.empty()) g_term_lines.emplace_back();
    if (static_cast<int>(g_term_lines.size()) < kTermRows) {
        g_term_lines.emplace_back();
    } else {
        g_term_lines.erase(g_term_lines.begin());
        g_term_lines.emplace_back();
    }
    g_term_col = 0;
}

void terminal_put_byte(unsigned char ch)
{
    if (g_term_lines.empty()) g_term_lines.emplace_back();
    if (ch == '\r') {
        g_term_col = 0;
    } else if (ch == '\n') {
        terminal_newline();
    } else if (ch == '\b' || ch == 0x7f) {
        if (g_term_col > 0) {
            --g_term_col;
            if (g_term_col < static_cast<int>(g_term_lines.back().size())) {
                g_term_lines.back().erase(g_term_col, 1);
            }
        }
    } else if (ch >= 0x20) {
        std::string &line = g_term_lines.back();
        if (g_term_col > static_cast<int>(line.size())) line.resize(g_term_col, ' ');
        if (g_term_col < static_cast<int>(line.size())) {
            line[g_term_col] = static_cast<char>(ch);
        } else {
            line.push_back(static_cast<char>(ch));
        }
        ++g_term_col;
        if (g_term_col >= kTermCols) terminal_newline();
    }
}

void terminal_apply_escape(const std::string &seq)
{
    if (seq.size() < 2) return;
    char final = seq.back();
    if (final == 'J') {
        terminal_clear();
    } else if (final == 'H' || final == 'f') {
        if (seq == "\033[H" || seq == "\033[1;1H" || seq == "\033[f") terminal_clear();
    } else if (final == 'K') {
        if (g_term_lines.empty()) g_term_lines.emplace_back();
        if (g_term_col < static_cast<int>(g_term_lines.back().size())) {
            g_term_lines.back().resize(g_term_col);
        }
    }
}

void terminal_feed(const char *buf, ssize_t len)
{
    for (ssize_t i = 0; i < len; ++i) {
        unsigned char ch = static_cast<unsigned char>(buf[i]);
        if (!g_term_escape.empty()) {
            g_term_escape.push_back(static_cast<char>(ch));
            if (ch >= 0x40 && ch <= 0x7e) {
                terminal_apply_escape(g_term_escape);
                g_term_escape.clear();
            } else if (g_term_escape.size() > 32) {
                g_term_escape.clear();
            }
            continue;
        }
        if (ch == 0x1b) {
            g_term_escape = "\033";
            continue;
        }
        terminal_put_byte(ch);
    }
}

void terminal_append_status(const std::string &text)
{
    if (!g_term_lines.empty() && !g_term_lines.back().empty()) terminal_newline();
    g_term_lines.push_back(text);
    g_term_col = static_cast<int>(text.size());
    while (static_cast<int>(g_term_lines.size()) > kTermRows) g_term_lines.erase(g_term_lines.begin());
}

void close_terminal_fd()
{
    if (g_term_fd >= 0) {
        close(g_term_fd);
        g_term_fd = -1;
    }
}

void stop_terminal_app(bool kill_child)
{
    if (kill_child && g_term_running && g_term_pid > 0) {
        kill(g_term_pid, SIGTERM);
        bool alive = true;
        for (int i = 0; i < 10; ++i) {
            int status = 0;
            pid_t done = waitpid(g_term_pid, &status, WNOHANG);
            if (done == g_term_pid) {
                alive = false;
                break;
            }
            usleep(20000);
        }
        if (alive) {
            kill(g_term_pid, SIGKILL);
            waitpid(g_term_pid, nullptr, WNOHANG);
        }
    }
    close_terminal_fd();
    g_term_pid = -1;
    g_term_running = false;
}

bool start_terminal_app(const std::string &title, const std::string &root, const std::vector<std::string> &argv,
                        Screen return_screen = Screen::Detail)
{
    if (argv.empty()) return false;
    stop_terminal_app(true);
    g_terminal_return_screen = return_screen;
    terminal_clear();
    g_term_title = title.empty() ? "App" : title;
    terminal_append_status("[Starting " + one_line(g_term_title, 40) + "]");

    struct winsize ws = {};
    ws.ws_row = kTermRows;
    ws.ws_col = kTermCols;
    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        terminal_append_status("Failed to start PTY");
        return false;
    }
    if (pid == 0) {
        if (!root.empty()) chdir(root.c_str());
        setenv("TERM", "xterm", 0);
        setenv("LINES", "16", 1);
        setenv("COLUMNS", "58", 1);
        std::vector<char *> child_argv;
        child_argv.reserve(argv.size() + 1);
        for (const std::string &arg : argv) child_argv.push_back(const_cast<char *>(arg.c_str()));
        child_argv.push_back(nullptr);
        execvp(child_argv[0], child_argv.data());
        perror("execvp");
        _exit(127);
    }

    g_term_fd = master_fd;
    g_term_pid = pid;
    g_term_running = true;
    int flags = fcntl(g_term_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(g_term_fd, F_SETFL, flags | O_NONBLOCK);
    if (!g_terminal_timer) g_terminal_timer = lv_timer_create(terminal_timer_cb, 40, nullptr);
    g_screen = Screen::Terminal;
    terminal_poll();
    return true;
}

void terminal_poll()
{
    bool changed = false;
    if (g_term_fd >= 0) {
        char buf[512];
        while (true) {
            ssize_t n = read(g_term_fd, buf, sizeof(buf));
            if (n > 0) {
                terminal_feed(buf, n);
                changed = true;
            } else if (n == 0) {
                close_terminal_fd();
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_terminal_fd();
                break;
            }
        }
    }
    if (g_term_running && g_term_pid > 0) {
        int status = 0;
        pid_t done = waitpid(g_term_pid, &status, WNOHANG);
        if (done == g_term_pid) {
            g_term_running = false;
            g_term_pid = -1;
            close_terminal_fd();
            terminal_append_status(WIFEXITED(status) ? "[Exited. Esc back]" : "[Stopped. Esc back]");
            changed = true;
        }
    }
    if (changed && g_screen == Screen::Terminal) render();
}

void terminal_timer_cb(lv_timer_t *)
{
    terminal_poll();
}

void terminal_send_key(const key_item *key)
{
    if (!key || key->key_state == KBD_KEY_RELEASED || g_term_fd < 0) return;
    const char *fallback = nullptr;
    if (key->utf8[0]) {
        write(g_term_fd, key->utf8, strlen(key->utf8));
        return;
    }
    switch (key->key_code) {
        case KEY_UP: fallback = "\033[A"; break;
        case KEY_DOWN: fallback = "\033[B"; break;
        case KEY_RIGHT: fallback = "\033[C"; break;
        case KEY_LEFT: fallback = "\033[D"; break;
        case KEY_ENTER: fallback = "\r"; break;
        case KEY_BACKSPACE: fallback = "\x7f"; break;
        case KEY_TAB: fallback = "\t"; break;
        default: break;
    }
    if (fallback) write(g_term_fd, fallback, strlen(fallback));
}

void refresh_summary()
{
    std::string cmd = "python3 " + shell_quote(g_script_path) + " --lvgl-summary";
    std::string output = run_capture(cmd);
    std::istringstream stream(output);
    std::string line;
    std::vector<Job> jobs;
    bool deps_seen = false;

    while (std::getline(stream, line)) {
        auto fields = split_tab(line);
        if (fields.empty()) continue;
        if (fields[0] == "DEP" && fields.size() >= 3) {
            deps_seen = true;
            g_deps_ok = fields[1] == "1";
            g_dep_status = g_deps_ok ? "ready" : ("missing " + fields[2]);
        } else if (fields[0] == "JOB" && fields.size() >= 9) {
            Job job;
            job.id = fields[1];
            job.name = fields[2];
            job.status = fields[3];
            job.installed = fields[4] == "1";
            job.session = fields[5];
            job.updated = fields[6];
            job.app_dir = fields[7];
            job.log_file = fields[8];
            if (fields.size() >= 10) job.message = fields[9];
            jobs.push_back(job);
        }
    }
    if (!deps_seen) {
        g_deps_ok = false;
        g_dep_status = "backend error";
    }
    g_jobs = jobs;
    if (g_selected >= static_cast<int>(g_jobs.size())) g_selected = static_cast<int>(g_jobs.size()) - 1;
    if (g_selected < 0) g_selected = 0;
}

std::string write_request_file(const std::string &text)
{
    std::string dir = home_state_dir();
    std::string mkdir_cmd = "mkdir -p " + shell_quote(dir + "/requests");
    std::system(mkdir_cmd.c_str());
    char name[128];
    std::snprintf(name, sizeof(name), "%ld-%d.txt", static_cast<long>(std::time(nullptr)), static_cast<int>(getpid()));
    std::string path = dir + "/requests/" + name;
    std::ofstream file(path);
    file << text;
    return path;
}

std::string started_job_id_from_output(const std::string &out)
{
    std::istringstream stream(out);
    std::string first_line;
    std::getline(stream, first_line);
    auto fields = split_tab(first_line);
    if (fields.size() >= 2 && fields[0] == "STARTED") return fields[1];
    return "";
}

void enter_frozen_wait(const std::string &job_id)
{
    if (!job_id.empty()) g_frozen_job_id = job_id;
    refresh_summary();
    select_job_by_id(g_frozen_job_id);
    g_status_message = "Low refresh mode";
    g_screen = Screen::FrozenWait;
}

void start_create_job()
{
    if (!g_deps_ok) {
        g_status_message = "Install missing deps first";
        return;
    }
    if (g_name_input.empty() || g_request_input.empty()) {
        g_status_message = "Name and request required";
        return;
    }
    std::string request_file = write_request_file(g_request_input);
    std::string cmd = "python3 " + shell_quote(g_script_path) +
        " --create-from-file " + shell_quote(g_name_input) + " " + shell_quote(request_file);
    std::string out = run_capture(cmd);
    std::string job_id = started_job_id_from_output(out);
    g_status_message = !job_id.empty() ? "Started. Freezing UI." : one_line(out, 52);
    if (!job_id.empty()) {
        g_name_input.clear();
        g_request_input.clear();
        enter_frozen_wait(job_id);
    } else {
        g_screen = Screen::InputRequest;
    }
}

void start_continue_job(const std::string &mode)
{
    if (g_jobs.empty() || g_selected >= static_cast<int>(g_jobs.size())) return;
    if (!g_deps_ok) {
        g_status_message = "Install missing deps first";
        return;
    }
    std::string text = g_modify_input.empty()
        ? "Continue and finish this APPLaunch app."
        : g_modify_input;
    std::string request_file = write_request_file(text);
    const Job &job = g_jobs[g_selected];
    std::string cmd = "python3 " + shell_quote(g_script_path) +
        " --continue-from-file " + shell_quote(job.id) + " " +
        shell_quote(mode) + " " + shell_quote(request_file);
    std::string out = run_capture(cmd);
    std::string job_id = started_job_id_from_output(out);
    g_status_message = !job_id.empty() ? "Started. Freezing UI." : one_line(out, 52);
    if (!job_id.empty()) {
        g_modify_input.clear();
        enter_frozen_wait(job_id);
    } else {
        g_screen = mode == "modify" ? Screen::Modify : Screen::Detail;
    }
}

void clear_pending_start()
{
    g_pending_action.clear();
    g_pending_job_id.clear();
    g_pending_name.clear();
    g_pending_request.clear();
}

void prepare_create_confirmation()
{
    if (!g_deps_ok) {
        g_status_message = "Install missing deps first";
        return;
    }
    if (g_name_input.empty() || g_request_input.empty()) {
        g_status_message = "Name and request required";
        return;
    }
    g_pending_action = "create";
    g_pending_job_id.clear();
    g_pending_name = g_name_input;
    g_pending_request = g_request_input;
    g_confirm_cancel_screen = Screen::InputRequest;
    g_status_message.clear();
    g_screen = Screen::ConfirmStart;
}

void prepare_continue_confirmation(const std::string &mode)
{
    if (g_jobs.empty() || g_selected >= static_cast<int>(g_jobs.size())) return;
    if (!g_deps_ok) {
        g_status_message = "Install missing deps first";
        return;
    }
    const Job &job = g_jobs[g_selected];
    g_pending_action = mode;
    g_pending_job_id = job.id;
    g_pending_name = job.name;
    g_pending_request = g_modify_input.empty()
        ? "Continue and finish this APPLaunch app."
        : g_modify_input;
    g_confirm_cancel_screen = mode == "modify" ? Screen::Modify : Screen::Detail;
    g_status_message.clear();
    g_screen = Screen::ConfirmStart;
}

void cancel_pending_start()
{
    clear_pending_start();
    g_status_message = "Cancelled";
    g_screen = g_confirm_cancel_screen;
}

void confirm_pending_start()
{
    if (g_pending_action == "create") {
        g_name_input = g_pending_name;
        g_request_input = g_pending_request;
        clear_pending_start();
        start_create_job();
        return;
    }
    if (g_pending_action == "continue" || g_pending_action == "modify") {
        std::string action = g_pending_action;
        std::string job_id = g_pending_job_id;
        g_modify_input = g_pending_request;
        clear_pending_start();
        select_job_by_id(job_id);
        start_continue_job(action);
        return;
    }
    cancel_pending_start();
}

void terminate_selected()
{
    if (g_jobs.empty() || g_selected >= static_cast<int>(g_jobs.size())) return;
    std::string cmd = "python3 " + shell_quote(g_script_path) + " --terminate " + shell_quote(g_jobs[g_selected].id);
    run_capture(cmd);
    g_status_message = "Terminated job";
    refresh_summary();
}

void terminate_running()
{
    std::string cmd = "python3 " + shell_quote(g_script_path) + " --terminate-running";
    run_capture(cmd);
    g_status_message = "Terminated running jobs";
    refresh_summary();
}

void run_selected()
{
    if (g_jobs.empty() || g_selected >= static_cast<int>(g_jobs.size())) return;
    std::string cmd = "python3 " + shell_quote(g_script_path) + " --run " + shell_quote(g_jobs[g_selected].id);
    std::string out = run_capture(cmd);
    std::istringstream stream(out);
    std::string first_line;
    std::getline(stream, first_line);
    auto fields = split_tab(first_line);
    if (fields.size() >= 4 && fields[0] == "TERMINAL") {
        std::vector<std::string> argv(fields.begin() + 3, fields.end());
        if (!start_terminal_app(fields[1], fields[2], argv)) {
            g_status_message = "Failed to run terminal app";
        }
    } else if (out.find("RUNNING") != std::string::npos) {
        g_status_message = "Launched app";
        request_quit();
    } else if (out.find("ERROR") != std::string::npos) {
        g_status_message = one_line(out, 52);
    } else {
        g_status_message = one_line(out.empty() ? "Run command finished" : out, 52);
    }
    refresh_summary();
}

void delete_selected()
{
    if (g_jobs.empty() || g_selected >= static_cast<int>(g_jobs.size())) return;
    std::string cmd = "python3 " + shell_quote(g_script_path) + " --delete " + shell_quote(g_jobs[g_selected].id);
    std::string out = run_capture(cmd);
    g_status_message = out.find("DELETED") != std::string::npos ? "Deleted app" : one_line(out, 52);
    refresh_summary();
    g_screen = Screen::Home;
}

void install_dependencies()
{
    std::vector<std::string> argv = {"python3", g_script_path, "--ensure-deps"};
    if (!start_terminal_app("Setup", g_app_dir, argv, Screen::Home)) {
        g_status_message = "Failed to start setup";
    }
}

bool has_running_job()
{
    for (const auto &job : g_jobs) {
        if (job.status == "running" || job.status == "queued") return true;
    }
    return false;
}

bool job_is_running(const Job &job)
{
    return job.status == "running" || job.status == "queued";
}

int find_job_index(const std::string &job_id)
{
    for (int i = 0; i < static_cast<int>(g_jobs.size()); ++i) {
        if (g_jobs[i].id == job_id) return i;
    }
    return -1;
}

const Job *frozen_job()
{
    int index = find_job_index(g_frozen_job_id);
    if (index >= 0) return &g_jobs[index];
    if (g_selected >= 0 && g_selected < static_cast<int>(g_jobs.size())) return &g_jobs[g_selected];
    return nullptr;
}

void select_job_by_id(const std::string &job_id)
{
    int index = find_job_index(job_id);
    if (index >= 0) g_selected = index;
}

void load_log_tail()
{
    g_log_lines.clear();
    if (g_jobs.empty() || g_selected >= static_cast<int>(g_jobs.size())) return;
    std::string cmd = "python3 " + shell_quote(g_script_path) + " --log-tail " +
        shell_quote(g_jobs[g_selected].id) + " --tail-count 24 --tail-offset " +
        std::to_string(g_log_offset) + " --compact-log";
    std::string out = run_capture(cmd);
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        if (g_log_wrap) {
            push_wrapped(g_log_lines, line, 54, 4);
        } else {
            g_log_lines.push_back(one_line(line, 56));
        }
        if (g_log_lines.size() > 80) {
            g_log_lines.erase(g_log_lines.begin(), g_log_lines.begin() + (g_log_lines.size() - 80));
        }
    }
}

void load_chat_tail()
{
    g_chat_entries.clear();
    if (g_jobs.empty() || g_selected >= static_cast<int>(g_jobs.size())) return;
    std::string cmd = "python3 " + shell_quote(g_script_path) + " --chat-tail " +
        shell_quote(g_jobs[g_selected].id) + " --tail-count 18 --tail-offset " +
        std::to_string(g_chat_offset);
    std::string out = run_capture(cmd);
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        auto fields = split_tab(line);
        if (fields.size() >= 4 && fields[0] == "CHAT") {
            ChatEntry entry;
            entry.time = fields[1];
            entry.role = fields[2];
            entry.text = fields[3];
            g_chat_entries.push_back(entry);
        }
    }
}

void clean_root()
{
    lv_obj_clean(g_root);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x080B10), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *label(lv_obj_t *parent, const std::string &text, int x, int y, int w, int h,
                const lv_font_t *font, uint32_t color, lv_label_long_mode_t mode = LV_LABEL_LONG_CLIP)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, mode);
    lv_label_set_text(obj, text.c_str());
    return obj;
}

void header(const std::string &title, const std::string &right)
{
    lv_obj_t *bar = lv_obj_create(g_root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, kScreenWidth, kHeaderHeight);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x111923), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    label(bar, title, 8, 5, 170, 15, &lv_font_montserrat_14, 0xF4F8FB);
    label(bar, right, 186, 6, 128, 13, &lv_font_montserrat_10,
          g_deps_ok ? 0x8BE28B : 0xFFB05F, LV_LABEL_LONG_DOT);
}

void render_home()
{
    clean_root();
    header("VibApp", g_dep_status);
    int y = 29;
    if (!g_deps_ok) {
        label(g_root, "Missing dependency.", 9, y, 302, 16, &lv_font_montserrat_12, 0xFFD18A);
        y += 18;
        label(g_root, one_line(g_dep_status, 44), 9, y, 302, 14, &lv_font_montserrat_10, 0xB8C4CD);
        y += 15;
        label(g_root, "Press I to install opencode + skill.", 9, y, 302, 14, &lv_font_montserrat_10, 0xB8C4CD);
        y += 18;
    } else if (g_jobs.empty()) {
        label(g_root, "No apps yet. Press N to create.", 9, y, 302, 16, &lv_font_montserrat_12, 0xD8E2EA);
        y += 18;
    } else {
        for (int i = 0; i < std::min<int>(5, g_jobs.size()); ++i) {
            const Job &job = g_jobs[i];
            uint32_t color = i == g_selected ? 0xFFFFFF : 0xB8C4CD;
            std::string installed = job.installed ? "*" : "-";
            std::string row = (i == g_selected ? ">" : " ") + one_line(job.name, 15) +
                " " + one_line(job.status, 8) + " " + installed;
            label(g_root, row, 8, y, 304, 14, &lv_font_montserrat_10, color);
            y += kLineHeight;
        }
    }
    for (const auto &job : g_jobs) {
        if (job.status == "completed" && job.installed) {
            label(g_root, "Ready: return to APPLaunch to test.", 8, 112, 304, 14,
                  &lv_font_montserrat_10, 0x7BE38C);
            break;
        }
    }
    if (!g_status_message.empty()) {
        label(g_root, one_line(g_status_message, 42), 8, 128, 304, 14,
              &lv_font_montserrat_10, 0xFFE08A);
    }
    label(g_root, g_deps_ok ? "N new  I setup  R refresh  Hold Esc quit" : "I setup  R refresh  Hold Esc quit",
          8, 153, 304, 13,
          &lv_font_montserrat_10, 0x7F8B96);
}

void render_text_input(const std::string &title, const std::string &text, bool name_field)
{
    clean_root();
    header(title, name_field ? "Enter next" : "Enter start");
    label(g_root, name_field ? "App name:" : "Request:", 8, 31, 90, 13, &lv_font_montserrat_10, 0x8EA0AC);

    lv_obj_t *box = lv_obj_create(g_root);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 8, 48);
    lv_obj_set_size(box, 304, name_field ? 34 : 78);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x121D29), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2E4153), 0);
    label(box, text.empty() ? "_" : text, 6, 6, 292, name_field ? 20 : 66,
          &lv_font_montserrat_12, 0xF5F7F9, name_field ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);

    if (name_field) {
        label(g_root, one_line(g_request_input.empty() ? "Then type the user request." : g_request_input, 44),
              8, 91, 304, 30, &lv_font_montserrat_10, 0xB6C2CC, LV_LABEL_LONG_WRAP);
    } else {
        label(g_root, one_line(g_name_input, 38), 8, 132, 304, 13, &lv_font_montserrat_10, 0x8EA0AC);
    }
    label(g_root, "Backspace edit  Esc cancel", 8, 153, 304, 13, &lv_font_montserrat_10, 0x7F8B96);
}

void render_detail()
{
    clean_root();
    header("Job", "B back");
    if (g_jobs.empty()) {
        label(g_root, "No selected job.", 8, 34, 304, 14, &lv_font_montserrat_12, 0xD8E2EA);
        return;
    }
    const Job &job = g_jobs[g_selected];
    label(g_root, one_line(job.name, 35), 8, 31, 304, 16, &lv_font_montserrat_12, 0xF4F8FB);
    label(g_root, "Status: " + job.status + (job.installed ? " / APPLaunch" : ""), 8, 52, 304, 13,
          &lv_font_montserrat_10, job.installed ? 0x7BE38C : 0xC7D2DA);
    label(g_root, "Session: " + one_line(job.session.empty() ? "-" : job.session, 30), 8, 68, 304, 13,
          &lv_font_montserrat_10, 0x9CADB9);
    label(g_root, "Dir: " + one_line(job.app_dir, 42), 8, 84, 304, 13, &lv_font_montserrat_10, 0x9CADB9);
    if (job.installed) {
        label(g_root, "Ready in APPLaunch.", 8, 102, 304, 13,
              &lv_font_montserrat_10, 0x8FE69B, LV_LABEL_LONG_WRAP);
    }
    if (!g_status_message.empty()) {
        label(g_root, one_line(g_status_message, 44), 8, 116, 304, 13,
              &lv_font_montserrat_10, 0xFFE08A);
    }
    label(g_root, "R run  D delete  V log  H chat", 8, 126, 304, 13,
          &lv_font_montserrat_10, 0xD0D8DF);
    label(g_root, "M modify  C continue  T stop", 8, 140, 304, 13,
          &lv_font_montserrat_10, 0xD0D8DF);
    label(g_root, "Esc/B back", 8, 154, 304, 13, &lv_font_montserrat_10, 0x7F8B96);
}

void render_log()
{
    clean_root();
    header("Log", std::string(g_log_wrap ? "wrap" : "clip") + (g_log_offset ? (" +" + std::to_string(g_log_offset)) : ""));
    int y = 30;
    if (g_log_lines.empty()) {
        label(g_root, "No log yet.", 8, y, 304, 14, &lv_font_montserrat_10, 0xCAD5DD);
    } else {
        int start = std::max<int>(0, static_cast<int>(g_log_lines.size()) - 8);
        for (int i = start; i < static_cast<int>(g_log_lines.size()); ++i) {
            const auto &line = g_log_lines[i];
            label(g_root, line, 8, y, 304, 13, &lv_font_montserrat_10, 0xCAD5DD);
            y += 14;
            if (y > 142) break;
        }
    }
    label(g_root, "Up old  Down new  W wrap  H chat", 8, 153, 304, 13, &lv_font_montserrat_10, 0x7F8B96);
}

int bubble_height(const ChatEntry &entry, int width)
{
    int chars = std::max(18, width / 6);
    int text_len = static_cast<int>(clean_inline(entry.text).size());
    int rows = std::max(1, std::min(4, (text_len + chars - 1) / chars));
    return 14 + rows * 11;
}

void render_chat_bubble(const ChatEntry &entry, int y)
{
    bool user = entry.role == "user";
    bool tool = entry.role == "tool";
    bool system = entry.role == "system";
    int x = user ? 76 : (tool || system ? 28 : 8);
    int w = user ? 236 : (tool || system ? 284 : 252);
    int h = bubble_height(entry, w);
    uint32_t bg = user ? 0x143522 : (tool ? 0x262B33 : (system ? 0x2B2416 : 0x122437));
    uint32_t border = user ? 0x287A45 : (tool ? 0x4B5563 : (system ? 0x775B22 : 0x2B6F9D));
    uint32_t text = user ? 0xD8F7DE : (tool ? 0xD7DEE5 : (system ? 0xFFE0A3 : 0xEAF5FF));

    lv_obj_t *box = lv_obj_create(g_root);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(border), 0);
    lv_obj_set_style_radius(box, 6, 0);

    std::string title = one_line((entry.time.empty() ? "" : entry.time + " ") + entry.role, 32);
    label(box, title, 6, 3, w - 12, 10, &lv_font_montserrat_10, 0x8FA0AD, LV_LABEL_LONG_DOT);
    label(box, clean_inline(entry.text), 6, 14, w - 12, h - 16,
          &lv_font_montserrat_10, text, g_chat_wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
}

void render_chat()
{
    clean_root();
    header("Chat", std::string(g_chat_wrap ? "wrap" : "clip") + (g_chat_offset ? (" +" + std::to_string(g_chat_offset)) : ""));
    if (g_chat_entries.empty()) {
        label(g_root, "No chat events yet.", 8, 34, 304, 14, &lv_font_montserrat_10, 0xCAD5DD);
    } else {
        int y = 149;
        for (int i = static_cast<int>(g_chat_entries.size()) - 1; i >= 0; --i) {
            int h = bubble_height(g_chat_entries[i], g_chat_entries[i].role == "user" ? 236 : 252);
            y -= h + 4;
            if (y < 28) break;
            render_chat_bubble(g_chat_entries[i], y);
        }
    }
    label(g_root, "Up old  Down new  W wrap  V log", 8, 153, 304, 13, &lv_font_montserrat_10, 0x7F8B96);
}

void render_confirm_start()
{
    clean_root();
    header("Resource Freeze", "Enter start");
    std::string name = g_pending_name.empty() ? "this app" : one_line(g_pending_name, 22);
    label(g_root, "Start creating " + name + "?", 8, 31, 304, 15,
          &lv_font_montserrat_12, 0xF4F8FB, LV_LABEL_LONG_DOT);
    label(g_root, "This device has limited resources.", 8, 53, 304, 13,
          &lv_font_montserrat_10, 0xFFD18A);
    label(g_root, "To save CPU, VibApp will enter", 8, 70, 304, 13,
          &lv_font_montserrat_10, 0xCAD5DD);
    label(g_root, "a frozen low-refresh screen.", 8, 84, 304, 13,
          &lv_font_montserrat_10, 0xCAD5DD);
    label(g_root, "It checks status once per minute.", 8, 102, 304, 13,
          &lv_font_montserrat_10, 0xCAD5DD);
    label(g_root, "Do not background VibApp while it runs.", 8, 119, 304, 13,
          &lv_font_montserrat_10, 0xFFB05F);
    label(g_root, "Enter/Y start   Esc/B cancel", 8, 153, 304, 13,
          &lv_font_montserrat_10, 0x7F8B96);
}

void render_frozen_wait()
{
    clean_root();
    const Job *job = frozen_job();
    bool running = job && job_is_running(*job);
    bool completed = job && job->status == "completed";
    bool failed = job && (job->status == "failed" || job->status == "unknown");
    header("VibApp Frozen", running ? "60s check" : "done");

    if (!job) {
        label(g_root, "Waiting for job state...", 8, 34, 304, 15,
              &lv_font_montserrat_12, 0xF4F8FB);
        label(g_root, "Next status check is once per minute.", 8, 58, 304, 13,
              &lv_font_montserrat_10, 0xCAD5DD);
    } else if (running) {
        label(g_root, "Creating: " + one_line(job->name, 24), 8, 31, 304, 15,
              &lv_font_montserrat_12, 0xF4F8FB, LV_LABEL_LONG_DOT);
        label(g_root, "Machine is in low-refresh mode.", 8, 55, 304, 13,
              &lv_font_montserrat_10, 0xCAD5DD);
        label(g_root, "Screen checks status every 1 minute.", 8, 72, 304, 13,
              &lv_font_montserrat_10, 0xCAD5DD);
        label(g_root, "Keep VibApp in foreground.", 8, 90, 304, 13,
              &lv_font_montserrat_10, 0xFFD18A);
        label(g_root, "Esc/quit is disabled while building.", 8, 108, 304, 13,
              &lv_font_montserrat_10, 0xFFB05F);
        label(g_root, "Status: " + job->status, 8, 130, 304, 13,
              &lv_font_montserrat_10, 0x8FE69B);
    } else if (completed) {
        label(g_root, "App development completed.", 8, 34, 304, 16,
              &lv_font_montserrat_12, 0x8FE69B);
        label(g_root, "App: " + one_line(job->name, 30), 8, 58, 304, 13,
              &lv_font_montserrat_10, 0xCAD5DD);
        label(g_root, job->installed ? "Installed in APPLaunch." : "Open detail to inspect package.",
              8, 76, 304, 13, &lv_font_montserrat_10, job->installed ? 0x8FE69B : 0xFFD18A);
        if (!job->message.empty()) {
            label(g_root, one_line(job->message, 50), 8, 98, 304, 26,
                  &lv_font_montserrat_10, 0x9CADB9, LV_LABEL_LONG_WRAP);
        }
    } else if (failed) {
        label(g_root, "App development failed.", 8, 34, 304, 16,
              &lv_font_montserrat_12, 0xFFB05F);
        label(g_root, "App: " + one_line(job->name, 30), 8, 58, 304, 13,
              &lv_font_montserrat_10, 0xCAD5DD);
        label(g_root, one_line(job->message.empty() ? ("Status: " + job->status) : job->message, 92),
              8, 78, 304, 42, &lv_font_montserrat_10, 0xFFD18A, LV_LABEL_LONG_WRAP);
    } else {
        label(g_root, "Job stopped: " + one_line(job->status, 20), 8, 38, 304, 15,
              &lv_font_montserrat_12, 0xFFD18A);
        if (!job->message.empty()) {
            label(g_root, one_line(job->message, 90), 8, 62, 304, 40,
                  &lv_font_montserrat_10, 0xCAD5DD, LV_LABEL_LONG_WRAP);
        }
    }

    if (running) {
        label(g_root, "Please wait. Display sleeps between checks.", 8, 153, 304, 13,
              &lv_font_montserrat_10, 0x7F8B96);
    } else {
        label(g_root, "Enter detail  R refresh", 8, 153, 304, 13,
              &lv_font_montserrat_10, 0x7F8B96);
    }
}

void render_exit_prompt()
{
    clean_root();
    header("Exit VibApp", "jobs running");
    label(g_root, "App creation is still running.", 8, 36, 304, 15, &lv_font_montserrat_12, 0xF4F8FB);
    label(g_root, "C: exit and keep building in background", 8, 62, 304, 13, &lv_font_montserrat_10, 0xBFD0DA);
    label(g_root, "T: terminate opencode job and exit", 8, 80, 304, 13, &lv_font_montserrat_10, 0xFFB05F);
    label(g_root, "S/Esc: stay in VibApp", 8, 98, 304, 13, &lv_font_montserrat_10, 0xBFD0DA);
}

void render_delete_prompt()
{
    clean_root();
    header("Delete App", "Y delete");
    if (g_jobs.empty()) {
        label(g_root, "No selected job.", 8, 34, 304, 14, &lv_font_montserrat_12, 0xD8E2EA);
        return;
    }
    const Job &job = g_jobs[g_selected];
    label(g_root, "Delete " + one_line(job.name, 24) + "?", 8, 36, 304, 16,
          &lv_font_montserrat_12, 0xF4F8FB);
    label(g_root, "This removes generated files, job state,", 8, 62, 304, 13,
          &lv_font_montserrat_10, 0xFFB05F);
    label(g_root, "and installed APPLaunch files if found.", 8, 78, 304, 13,
          &lv_font_montserrat_10, 0xFFB05F);
    label(g_root, "Y: delete   N/Esc/B: cancel", 8, 116, 304, 13,
          &lv_font_montserrat_10, 0xD0D8DF);
}

void render_terminal()
{
    clean_root();
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x000000), 0);
    for (int i = 0; i < kTermRows; ++i) {
        std::string line;
        if (i < static_cast<int>(g_term_lines.size())) line = one_line(g_term_lines[i], kTermCols);
        label(g_root, line, 2, i * 10, 316, 12, &lv_font_montserrat_10, 0xE7EDF2);
    }
    if (!g_term_running) {
        label(g_root, "Esc back", 248, 157, 68, 11, &lv_font_montserrat_10, 0xFFE08A);
    }
}

void render()
{
    if (g_refresh_timer) {
        uint32_t period = 4000;
        if (g_screen == Screen::FrozenWait) {
            const Job *job = frozen_job();
            period = (job && job_is_running(*job)) ? 60000 : 15000;
        } else if (g_screen == Screen::Home || g_screen == Screen::Detail) {
            period = has_running_job() ? 8000 : 15000;
        } else if (g_screen == Screen::Log || g_screen == Screen::Chat) {
            period = 60000;
        } else if (g_screen == Screen::InputName || g_screen == Screen::InputRequest || g_screen == Screen::Modify) {
            period = 30000;
        }
        lv_timer_set_period(g_refresh_timer, period);
    }

    switch (g_screen) {
        case Screen::Home: render_home(); break;
        case Screen::InputName: render_text_input("New App", g_name_input, true); break;
        case Screen::InputRequest: render_text_input("Describe App", g_request_input, false); break;
        case Screen::Detail: render_detail(); break;
        case Screen::Log: render_log(); break;
        case Screen::Chat: render_chat(); break;
        case Screen::ConfirmStart: render_confirm_start(); break;
        case Screen::FrozenWait: render_frozen_wait(); break;
        case Screen::Modify: render_text_input("Modify App", g_modify_input, false); break;
        case Screen::ExitPrompt: render_exit_prompt(); break;
        case Screen::DeletePrompt: render_delete_prompt(); break;
        case Screen::Terminal: render_terminal(); break;
    }
}

void refresh_timer_cb(lv_timer_t *)
{
    if (g_screen == Screen::Home || g_screen == Screen::Detail) {
        refresh_summary();
        render();
    } else if (g_screen == Screen::Log) {
        load_log_tail();
        render();
    } else if (g_screen == Screen::Chat) {
        load_chat_tail();
        render();
    } else if (g_screen == Screen::FrozenWait) {
        refresh_summary();
        select_job_by_id(g_frozen_job_id);
        render();
    }
}

char shortcut_char(const key_item *key)
{
    if (!key || key->key_state == KBD_KEY_RELEASED) return 0;
    if (key->utf8[0] && !key->utf8[1] && std::isprint(static_cast<unsigned char>(key->utf8[0]))) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(key->utf8[0])));
    }
    return 0;
}

bool has_printable_text(const key_item *key)
{
    if (!key || !key->utf8[0]) return false;
    unsigned char first = static_cast<unsigned char>(key->utf8[0]);
    return first >= 32 && first != 127;
}

bool is_control_key(const key_item *key, uint32_t code)
{
    return key && !has_printable_text(key) && key->key_code == code;
}

bool append_text_from_key(const key_item *key, std::string &target)
{
    if (!key || key->key_state == KBD_KEY_RELEASED) return false;
    if (has_printable_text(key)) {
        if (target.size() < 420) target += key->utf8;
        return true;
    }
    if (is_control_key(key, KEY_BACKSPACE)) {
        if (!target.empty()) target.pop_back();
        return true;
    }
    return false;
}

void handle_short_escape()
{
    switch (g_screen) {
        case Screen::Home:
            g_status_message = "Hold Esc to exit";
            break;
        case Screen::InputName:
        case Screen::InputRequest:
            g_screen = Screen::Home;
            break;
        case Screen::Detail:
            g_screen = Screen::Home;
            break;
        case Screen::Log:
            g_screen = Screen::Detail;
            break;
        case Screen::Chat:
            g_screen = Screen::Detail;
            break;
        case Screen::ConfirmStart:
            cancel_pending_start();
            break;
        case Screen::FrozenWait:
            g_status_message = "Keep VibApp open while building";
            break;
        case Screen::Modify:
            g_screen = Screen::Detail;
            break;
        case Screen::ExitPrompt:
            g_screen = Screen::Home;
            break;
        case Screen::DeletePrompt:
            g_screen = Screen::Detail;
            break;
        case Screen::Terminal:
            stop_terminal_app(true);
            g_status_message = "Stopped app";
            g_screen = g_terminal_return_screen;
            refresh_summary();
            break;
    }
}

void handle_long_escape()
{
    if (g_screen == Screen::Terminal) {
        stop_terminal_app(true);
    } else if (g_screen == Screen::FrozenWait) {
        g_status_message = "Background exit disabled";
        return;
    } else if (g_screen == Screen::ConfirmStart) {
        cancel_pending_start();
        return;
    }
    request_quit();
}

bool handle_escape_key_event(const key_item *key)
{
    if (!key || key->key_code != KEY_ESC) return false;

    if (key->key_state == KBD_KEY_PRESSED) {
        g_escape_pressed_tick = lv_tick_get();
        g_escape_long_fired = false;
        return true;
    }

    if (key->key_state == KBD_KEY_REPEATED) {
        if (!g_escape_pressed_tick) g_escape_pressed_tick = lv_tick_get();
        if (!g_escape_long_fired && lv_tick_elaps(g_escape_pressed_tick) >= kEscapeLongPressMs) {
            g_escape_long_fired = true;
            handle_long_escape();
        }
        return true;
    }

    if (key->key_state == KBD_KEY_RELEASED) {
        if (!g_escape_pressed_tick) g_escape_pressed_tick = lv_tick_get();
        if (!g_escape_long_fired && lv_tick_elaps(g_escape_pressed_tick) >= kEscapeLongPressMs) {
            handle_long_escape();
        } else if (!g_escape_long_fired) {
            handle_short_escape();
        }
        g_escape_pressed_tick = 0;
        g_escape_long_fired = false;
        return true;
    }

    return true;
}

void handle_home_key(const key_item *key)
{
    char ch = shortcut_char(key);
    if (is_control_key(key, KEY_UP) && g_selected > 0) {
        --g_selected;
    } else if (is_control_key(key, KEY_DOWN) && g_selected + 1 < static_cast<int>(g_jobs.size())) {
        ++g_selected;
    } else if (is_control_key(key, KEY_ENTER) && !g_jobs.empty()) {
        g_screen = Screen::Detail;
    } else if (ch == 'n') {
        g_status_message.clear();
        g_screen = Screen::InputName;
    } else if (ch == 'j' && !g_jobs.empty()) {
        g_screen = Screen::Detail;
    } else if (ch == 'i') {
        install_dependencies();
    } else if (ch == 'r') {
        refresh_summary();
    } else if (ch == 'q') {
        refresh_summary();
        g_screen = has_running_job() ? Screen::ExitPrompt : Screen::Home;
        if (!has_running_job()) request_quit();
    }
}

void handle_key_event(lv_event_t *event)
{
    key_item *key = static_cast<key_item *>(lv_event_get_param(event));
    if (!key) return;
    if (handle_escape_key_event(key)) {
        render();
        return;
    }
    if (key->key_state == KBD_KEY_RELEASED) return;
    char ch = shortcut_char(key);

    switch (g_screen) {
        case Screen::Home:
            handle_home_key(key);
            break;
        case Screen::InputName:
            if (is_control_key(key, KEY_ESC)) {
                g_screen = Screen::Home;
            } else if (is_control_key(key, KEY_ENTER) || is_control_key(key, KEY_TAB)) {
                g_screen = Screen::InputRequest;
            } else {
                append_text_from_key(key, g_name_input);
            }
            break;
        case Screen::InputRequest:
            if (is_control_key(key, KEY_ESC)) {
                g_screen = Screen::Home;
            } else if (is_control_key(key, KEY_ENTER)) {
                prepare_create_confirmation();
            } else if (is_control_key(key, KEY_TAB)) {
                g_screen = Screen::InputName;
            } else {
                append_text_from_key(key, g_request_input);
            }
            break;
        case Screen::Detail:
            if (is_control_key(key, KEY_ESC) || ch == 'b') {
                g_screen = Screen::Home;
            } else if (ch == 'v') {
                g_log_offset = 0;
                load_log_tail();
                g_screen = Screen::Log;
            } else if (ch == 'h') {
                g_chat_offset = 0;
                load_chat_tail();
                g_screen = Screen::Chat;
            } else if (ch == 'r') {
                run_selected();
            } else if (ch == 'd') {
                g_screen = Screen::DeletePrompt;
            } else if (ch == 'm') {
                g_modify_input.clear();
                g_screen = Screen::Modify;
            } else if (ch == 'c') {
                g_modify_input = "Continue and finish this APPLaunch app.";
                prepare_continue_confirmation("continue");
            } else if (ch == 't') {
                terminate_selected();
            }
            break;
        case Screen::Log:
            if (is_control_key(key, KEY_ESC) || ch == 'b') {
                g_screen = Screen::Detail;
            } else if (ch == 'r') {
                load_log_tail();
            } else if (ch == 'w') {
                g_log_wrap = !g_log_wrap;
                load_log_tail();
            } else if (ch == 'h') {
                g_chat_offset = 0;
                load_chat_tail();
                g_screen = Screen::Chat;
            } else if (is_control_key(key, KEY_UP)) {
                g_log_offset += 8;
                load_log_tail();
            } else if (is_control_key(key, KEY_DOWN)) {
                g_log_offset = std::max(0, g_log_offset - 8);
                load_log_tail();
            } else if (is_control_key(key, KEY_LEFT)) {
                g_log_offset += 32;
                load_log_tail();
            } else if (is_control_key(key, KEY_RIGHT)) {
                g_log_offset = std::max(0, g_log_offset - 32);
                load_log_tail();
            }
            break;
        case Screen::Chat:
            if (is_control_key(key, KEY_ESC) || ch == 'b') {
                g_screen = Screen::Detail;
            } else if (ch == 'r') {
                load_chat_tail();
            } else if (ch == 'w') {
                g_chat_wrap = !g_chat_wrap;
                load_chat_tail();
            } else if (ch == 'v') {
                g_log_offset = 0;
                load_log_tail();
                g_screen = Screen::Log;
            } else if (is_control_key(key, KEY_UP)) {
                g_chat_offset += 6;
                load_chat_tail();
            } else if (is_control_key(key, KEY_DOWN)) {
                g_chat_offset = std::max(0, g_chat_offset - 6);
                load_chat_tail();
            } else if (is_control_key(key, KEY_LEFT)) {
                g_chat_offset += 18;
                load_chat_tail();
            } else if (is_control_key(key, KEY_RIGHT)) {
                g_chat_offset = std::max(0, g_chat_offset - 18);
                load_chat_tail();
            }
            break;
        case Screen::Modify:
            if (is_control_key(key, KEY_ESC)) {
                g_screen = Screen::Detail;
            } else if (is_control_key(key, KEY_ENTER)) {
                prepare_continue_confirmation("modify");
            } else {
                append_text_from_key(key, g_modify_input);
            }
            break;
        case Screen::ConfirmStart:
            if (is_control_key(key, KEY_ENTER) || ch == 'y') {
                confirm_pending_start();
            } else if (is_control_key(key, KEY_ESC) || ch == 'b' || ch == 'n') {
                cancel_pending_start();
            }
            break;
        case Screen::FrozenWait: {
            const Job *job = frozen_job();
            bool running = job && job_is_running(*job);
            if (!running && (is_control_key(key, KEY_ENTER) || ch == 'j' || ch == 'b')) {
                if (job) select_job_by_id(job->id);
                g_screen = Screen::Detail;
            } else if (!running && ch == 'r') {
                refresh_summary();
                select_job_by_id(g_frozen_job_id);
            } else if (running) {
                g_status_message = "Wait for the 1 minute check";
            }
            break;
        }
        case Screen::ExitPrompt:
            if (ch == 'c') {
                request_quit();
            } else if (ch == 't') {
                terminate_running();
                request_quit();
            } else if (is_control_key(key, KEY_ESC) || ch == 's') {
                g_screen = Screen::Home;
            }
            break;
        case Screen::DeletePrompt:
            if (ch == 'y') {
                delete_selected();
            } else if (is_control_key(key, KEY_ESC) || ch == 'b' || ch == 'n') {
                g_screen = Screen::Detail;
            }
            break;
        case Screen::Terminal:
            if (is_control_key(key, KEY_ESC)) {
                stop_terminal_app(true);
                g_status_message = "Stopped app";
                g_screen = g_terminal_return_screen;
                refresh_summary();
            } else {
                terminal_send_key(key);
            }
            break;
    }
    render();
}

int get_st7789v_fbdev(char *dev_path, size_t buf_size)
{
    if (!dev_path || buf_size == 0) return -1;
    FILE *fp = std::fopen("/proc/fb", "r");
    if (!fp) return -1;
    char line[256];
    int fb_num = -1;
    while (std::fgets(line, sizeof(line), fp)) {
        if (std::strstr(line, "fb_st7789v") && std::sscanf(line, "%d", &fb_num) == 1) break;
    }
    std::fclose(fp);
    if (fb_num < 0) return -1;
    std::snprintf(dev_path, buf_size, "/dev/fb%d", fb_num);
    return 0;
}

#if LV_USE_EVDEV
int evdev_to_lv_key(uint16_t code)
{
    switch (code) {
        case KEY_UP: return LV_KEY_UP;
        case KEY_DOWN: return LV_KEY_DOWN;
        case KEY_RIGHT: return LV_KEY_RIGHT;
        case KEY_LEFT: return LV_KEY_LEFT;
        case KEY_ESC: return LV_KEY_ESC;
        case KEY_DELETE: return LV_KEY_DEL;
        case KEY_BACKSPACE: return LV_KEY_BACKSPACE;
        case KEY_ENTER: return LV_KEY_ENTER;
        case KEY_TAB: return KEY_TAB;
        case KEY_HOME: return LV_KEY_HOME;
        case KEY_END: return LV_KEY_END;
        default: return code;
    }
}

void keypad_read_cb(lv_indev_t *, lv_indev_data_t *data)
{
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    pthread_mutex_lock(&keyboard_mutex);
    if (!STAILQ_EMPTY(&keyboard_queue)) {
        key_item *elm = STAILQ_FIRST(&keyboard_queue);
        STAILQ_REMOVE_HEAD(&keyboard_queue, entries);
        if (g_root) {
            lv_obj_send_event(g_root, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), elm);
        }
        data->key = evdev_to_lv_key(elm->key_code);
        data->state = static_cast<lv_indev_state_t>(elm->key_state);
        data->continue_reading = !STAILQ_EMPTY(&keyboard_queue);
        std::free(elm);
    }
    pthread_mutex_unlock(&keyboard_mutex);
}

void lv_linux_indev_init()
{
    const char *keyboard_device = getenv_default(
        "LV_LINUX_KEYBOARD_DEVICE",
        "/dev/input/by-path/platform-3f804000.i2c-event");
    pthread_t thread_id;
    pthread_create(&thread_id, nullptr, keyboard_read_thread, const_cast<char *>(keyboard_device));
    pthread_detach(thread_id);
    g_keyboard_indev = lv_indev_create();
    lv_indev_set_type(g_keyboard_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(g_keyboard_indev, keypad_read_cb);
}
#endif

#if LV_USE_LINUX_FBDEV
void lv_linux_disp_init()
{
    char fbdev[64] = {};
    const char *device = getenv_default("LV_LINUX_FBDEV_DEVICE", nullptr);
    if (!device && get_st7789v_fbdev(fbdev, sizeof(fbdev)) == 0) device = fbdev;
    if (!device) device = "/dev/fb0";
    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp) lv_linux_fbdev_set_file(disp, device);
}

#if !LV_USE_EVDEV && !LV_USE_LIBINPUT
void lv_linux_indev_init() {}
#endif

#elif LV_USE_SDL
void lv_linux_disp_init()
{
    lv_display_t *disp = lv_sdl_window_create(kScreenWidth, kScreenHeight);
    lv_sdl_window_set_title(disp, "VibApp");
}

void lv_linux_indev_init()
{
    lv_sdl_mouse_create();
    g_keyboard_indev = lv_sdl_keyboard_create();
}
#else
#error Unsupported display configuration
#endif

void build_ui()
{
    g_root = lv_screen_active();
    lv_obj_set_size(g_root, kScreenWidth, kScreenHeight);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_root, handle_key_event, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), nullptr);

    g_group = lv_group_create();
    lv_group_add_obj(g_group, g_root);
    lv_group_focus_obj(g_root);
    if (g_keyboard_indev) lv_indev_set_group(g_keyboard_indev, g_group);
}

}  // namespace

int main(int argc, char **argv)
{
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    g_app_dir = dirname_of(argv && argv[0] ? argv[0] : nullptr);
    const char *script_env = std::getenv("VIBAPP_SCRIPT");
    g_script_path = script_env && script_env[0] ? script_env : (g_app_dir + "/vibapp.py");

    lv_init();
    lv_linux_disp_init();
    LV_EVENT_KEYBOARD = lv_event_register_id();
    lv_linux_indev_init();
    build_ui();
    refresh_summary();
    render();
    g_refresh_timer = lv_timer_create(refresh_timer_cb, 4000, nullptr);

    while (!g_quit_requested) {
        uint32_t next_ms = lv_timer_handler();
        uint32_t sleep_ms = 30;
        if (g_screen == Screen::Terminal) {
            sleep_ms = 8;
        } else if (g_screen == Screen::InputName || g_screen == Screen::InputRequest ||
                   g_screen == Screen::Modify || g_screen == Screen::ConfirmStart) {
            sleep_ms = 16;
        } else if (g_screen == Screen::FrozenWait) {
            sleep_ms = 80;
        } else if (g_screen == Screen::Log || g_screen == Screen::Chat) {
            sleep_ms = 50;
        }
        if (next_ms > 0) sleep_ms = std::min(sleep_ms, std::max<uint32_t>(1, next_ms));
        usleep(sleep_ms * 1000);
    }

    if (g_refresh_timer) lv_timer_delete(g_refresh_timer);
    if (g_terminal_timer) lv_timer_delete(g_terminal_timer);
    stop_terminal_app(true);
    return 0;
}
