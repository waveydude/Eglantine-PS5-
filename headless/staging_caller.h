#pragma once

namespace Eden::StagingCaller {
inline thread_local const char* current = "unspecified";

class Scope {
public:
    explicit Scope(const char* caller) noexcept : previous{current} {
        current = caller;
    }
    ~Scope() { current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* previous;
};
} // namespace Eden::StagingCaller
