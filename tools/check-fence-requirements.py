#!/usr/bin/env python3
"""Exercise actual fence commit/signal code and diagnostic masks with mock caches."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Eglantine/src/video_core/fence_manager.h').read_text()
diagnostic_header = root / 'Eglantine/src/video_core/readback_diagnostics.h'
if not diagnostic_header.is_file() or 'ReadbackDiagnostics' not in source:
    # The readback diagnostics this check drives belong to another core line; this core's fence
    # manager has none, so there is nothing of it to exercise.
    print('Fence requirements SKIP: this core has no ReadbackDiagnostics in fence_manager.h')
    raise SystemExit(0)
diagnostic = diagnostic_header.read_text()


def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


code = r'''
#include <array>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>
using u64=unsigned long long;using u8=unsigned char;
template<class F>struct Exit { F fn;~Exit(){fn();} };
struct ExitMaker { template<class F>auto operator+(F fn){return Exit<F>{std::move(fn)};} };
#define SCOPE_EXIT auto exit=ExitMaker{}+[&]()
namespace Settings {
inline bool delayed{};
bool IsGPUFenceBehaviorDefault(){return false;}bool IsGPULevelHigh(){return false;}
bool IsGPUFenceBehaviorBalanced(){return delayed;}bool IsGPUFenceBehaviorAccurate(){return false;}
}
namespace VideoCommon {
namespace Nx2Profile {enum class Zone{Fence,FenceSkipped};struct Scope{Scope(Zone){}};}
namespace ReadbackDiagnostics {
enum class Event { Count };
struct Measurement {};
inline bool enabled=true;
bool Enabled(){return enabled;}
'''
code += body(diagnostic, 'struct ThreadData') + ';\ninline thread_local ThreadData data;\n'
code += body(diagnostic, 'inline void CountFence') + '\n}\n'
code += r'''
struct Cache {
 std::mutex mutex;bool pending{},side_work{};unsigned checks{},commits{};
 bool HasUncommittedFlushes(){++checks;return pending;}
 bool HasAsyncFlushWork(){return pending||side_work;}
 void CommitAsyncFlushes(){++commits;pending=false;}
};
struct Gpu {unsigned begins{},ends{};void BeginAsyncGuestWrite(){++begins;}void EndAsyncGuestWrite(){++ends;}};
struct Raster {unsigned invalidates{},flushes{};void InvalidateGPUCache(){++invalidates;}void FlushCommands(){++flushes;}};
struct TestFence {bool stubbed;};
struct Manager {
 using TFence=std::shared_ptr<TestFence>;
 static constexpr bool can_async_check=false;
 static constexpr u8 TextureFlushes=1,BufferFlushes=2;
 Cache texture_cache,buffer_cache,query_cache;Gpu gpu;Raster rasterizer;
 std::mutex guard,uncommitted_guard;std::condition_variable cv;
 unsigned skipped_fences{},queued{};bool skip{};
 std::vector<std::function<void()>> uncommitted_operations;
 std::vector<std::vector<std::function<void()>>> pending_operations;
 std::vector<u8> pending_flushes;std::queue<TFence> fences;
 template<bool>void TryReleasePendingFences(){}
 bool SkipEmptyFence(){return skip;}
 TFence CreateFence(bool stubbed){return std::make_shared<TestFence>(TestFence{stubbed});}
 void QueueFence(const TFence&){++queued;}
 struct CacheFlushes{bool should_flush;u8 committed;u8 required;};
'''
code += body(source, 'CacheFlushes CommitCacheFlushes()') + '\n'
code += body(source, 'void SignalFence(std::function<void()>&& func)') + '\n};\n}\n'
code += r'''
int main(){
 using namespace VideoCommon;
 for(bool delayed:{false,true})for(unsigned mask=0;mask<8;++mask){
  Settings::delayed=delayed;ReadbackDiagnostics::data={};Manager m;
  m.texture_cache.pending=mask&1;m.buffer_cache.pending=mask&2;m.query_cache.pending=mask&4;
  unsigned callbacks{};m.SignalFence([&]{++callbacks;});
  assert(m.queued==1&&m.fences.size()==1&&m.fences.front()->stubbed==(mask==0));
  assert(ReadbackDiagnostics::data.fence_masks[mask]==1&&!ReadbackDiagnostics::data.skipped_empty_fences);
  assert(m.texture_cache.checks==1&&m.buffer_cache.checks==1&&m.query_cache.checks==1);
  assert(m.texture_cache.commits==bool(mask&1)&&m.buffer_cache.commits==bool(mask&2)&&m.query_cache.commits==1);
  assert(m.pending_flushes.back()==(mask&3)&&m.rasterizer.flushes==bool(mask));
  assert(callbacks==!delayed&&m.rasterizer.invalidates==1&&m.gpu.begins==m.gpu.ends);
  if(delayed){assert(m.pending_operations.back().size()==1);m.pending_operations.back()[0]();assert(callbacks==1);}
 }
 Settings::delayed=false;ReadbackDiagnostics::data={};
 {Manager m;m.skip=true;unsigned calls{};m.SignalFence([&]{++calls;});
  assert(calls==1&&!m.queued&&ReadbackDiagnostics::data.skipped_empty_fences==1);
  assert(!m.query_cache.checks&&!m.query_cache.commits&&m.gpu.begins==m.gpu.ends);}
 ReadbackDiagnostics::data={};
 {Manager m;m.texture_cache.side_work=true;unsigned calls{};m.SignalFence([&]{++calls;});
  assert(m.queued==1&&m.fences.front()->stubbed&&m.texture_cache.commits==1);
  assert(m.pending_flushes.back()==1&&!m.rasterizer.flushes&&ReadbackDiagnostics::data.fence_masks[0]==1);}
 ReadbackDiagnostics::enabled=false;ReadbackDiagnostics::data={};
 {Manager m;m.buffer_cache.pending=true;m.SignalFence([]{});assert(m.queued==1&&m.rasterizer.flushes==1);}
 for(auto n:ReadbackDiagnostics::data.fence_masks)assert(n==0);
}
'''
# SignalFence names cv in its discarded asynchronous branch.
code = '#include <condition_variable>\n' + code
with tempfile.TemporaryDirectory(prefix='fence-requirements-') as temp:
    path = Path(temp)
    (path / 'check.cpp').write_text(code)
    subprocess.run(['clang++-18', '-std=c++20', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True, timeout=10)
print('Fence requirements PASS: all masks in immediate/delayed modes, precommit requirements, empty skip, side-work stub, disabled counters and callback ordering')
