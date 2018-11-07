#pragma once

#include <sstream>
#include <string>

namespace hlt {
namespace log {

template <typename T>
std::string print(T arg) {
    std::stringstream ss;
    ss << arg;
    return ss.str();
}
template <typename T, typename... Ts>
std::string print(T arg, Ts... args) {
    std::stringstream ss;
    ss << arg << " " << print(args...);
    return ss.str();
}

void open(int bot_id);
void log(const std::string& message);

template <typename T, typename... Ts>
void log(T arg, Ts... args) {
    log(print(arg, args...));
}

}  // namespace log
}  // namespace hlt
