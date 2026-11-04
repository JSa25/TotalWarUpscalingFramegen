// Standalone integration test: no game DLL, FSR, Streamline or production
// transport.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

static const DWORD timeout_ms = 10000;
struct Handle {
  HANDLE h = nullptr;
  Handle() = default;
  explicit Handle(HANDLE v) : h(v) {}
  ~Handle() {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
struct Child {
  HANDLE h;
  explicit Child(HANDLE v) : h(v) {}
  ~Child() {
    if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
      TerminateProcess(h, 124);
      WaitForSingleObject(h, timeout_ms);
    }
    CloseHandle(h);
  }
};
template <class T> struct Com {
  T *p = nullptr;
  ~Com() {
    if (p)
      p->Release();
  }
  T *operator->() const { return p; }
  T **out() { return &p; }
  Com() = default;
  Com(const Com &) = delete;
};
[[noreturn]] static void fail(const char *resource, const char *api,
                              unsigned long code) {
  fprintf(stderr, "FAIL resource=%s API=%s error=0x%08lx\n", resource, api,
          code);
  throw std::runtime_error(api);
}
static void hr(HRESULT r, const char *resource, const char *api) {
  if (FAILED(r))
    fail(resource, api, (unsigned long)r);
}
static void win(bool ok, const char *resource, const char *api) {
  if (!ok)
    fail(resource, api, GetLastError());
}
#define HR(call, resource) hr((call), resource, #call)
#define WIN(call, resource) win(!!(call), resource, #call)
struct Wire {
  uint32_t version, size, pid, colour;
  LUID luid;
  uint64_t handles[10];
};
static_assert(sizeof(Wire) == 104, "cross-architecture wire layout");
static_assert(offsetof(Wire, handles) == 24, "fixed handle offset");
static const char *names[10] = {
    "game-fence", "sr-fence", "present-fence", "final",  "hudless",
    "depth",      "motion",   "srout0",        "srout1", "reactive"};
[[maybe_unused]] static DXGI_FORMAT format(int i, DXGI_FORMAT colour) {
  return i == 2   ? DXGI_FORMAT_R32_FLOAT
         : i == 3 ? DXGI_FORMAT_R16G16_FLOAT
         : i == 6 ? DXGI_FORMAT_R8_UNORM
                  : colour;
}
static const unsigned char input_bytes[] = {0x11, 0x22, 0x33, 0x44,
                                            0x55, 0x66, 0x77};
static const unsigned char output_bytes[] = {0x91, 0xa2};
static unsigned stride(int i) { return i == 6 ? 1 : 4; }
static std::wstring resource_name(uint32_t pid, int i) {
  return L"Local\\TWFGProbe_" + std::to_wstring(pid) + L"_" +
         std::to_wstring(i);
}
static void adapter_log(IDXGIAdapter1 *a) {
  DXGI_ADAPTER_DESC1 d{};
  HR(a->GetDesc1(&d), "adapter");
  fprintf(stderr,
          "adapter=%ls vendor=%04x device=%04x LUID=%08lx:%08lx bits=%zu\n",
          d.Description, d.VendorId, d.DeviceId,
          (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart,
          sizeof(void *) * 8);
}
[[maybe_unused]] static void wait11(ID3D11Fence *f, uint64_t value,
                                    const char *name) {
  DWORD start = GetTickCount();
  for (;;) {
    uint64_t v = f->GetCompletedValue();
    if (v == UINT64_MAX)
      fail(name, "GetCompletedValue(device removed)", ERROR_DEVICE_REMOVED);
    if (v >= value)
      return;
    if (GetTickCount() - start > timeout_ms)
      fail(name, "fence timeout", WAIT_TIMEOUT);
    Sleep(1);
  }
}
[[maybe_unused]] static void wait12(ID3D12Fence *f, uint64_t value,
                                    const char *name) {
  Handle e(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  WIN(e.h, name);
  HR(f->SetEventOnCompletion(value, e.h), name);
  DWORD w = WaitForSingleObject(e.h, timeout_ms);
  if (w != WAIT_OBJECT_0)
    fail(name, "WaitForSingleObject", w);
  if (f->GetCompletedValue() == UINT64_MAX)
    fail(name, "device removed", ERROR_DEVICE_REMOVED);
}
struct Mapping {
  Handle handle;
  Wire *wire = nullptr;
  ~Mapping() {
    if (wire)
      UnmapViewOfFile(wire);
  }
};
static void verify(const unsigned char *p, size_t pitch, unsigned bytes,
                   unsigned char expected, const char *name) {
  for (unsigned y = 0; y < 8; y++)
    for (unsigned x = 0; x < 8 * bytes; x++)
      if (p[y * pitch + x] != expected)
        fail(name, "texture contents", ERROR_CRC);
  fprintf(stderr, "PASS contents resource=%s expected=0x%02x\n", name,
          expected);
}

#ifndef _WIN64
static int producer(const wchar_t *consumer, DXGI_FORMAT colour, int cycles,
                    const wchar_t *mode) {
  Com<IDXGIFactory4> fac;
  HR(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)fac.out()),
     "factory");
  Com<IDXGIAdapter1> adapter;
  HR(fac->EnumAdapters1(0, adapter.out()), "adapter");
  adapter_log(adapter.p);
  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  HR(D3D11CreateDevice(adapter.p, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr,
                       0, D3D11_SDK_VERSION, dev.out(), nullptr, ctx.out()),
     "producer");
  Com<ID3D11Device5> d5;
  HR(dev->QueryInterface(__uuidof(ID3D11Device5), (void **)d5.out()),
     "producer");
  Com<ID3D11DeviceContext4> c4;
  HR(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)c4.out()),
     "producer");
  for (int cycle = 0; cycle < cycles; cycle++) {
    DWORD before = 0;
    WIN(GetProcessHandleCount(GetCurrentProcess(), &before), "producer");
    {
      fprintf(stderr, "cycle=%d begin\n", cycle);
      Mapping map;
      std::wstring mn = resource_name(GetCurrentProcessId(), 99);
      map.handle.h =
          CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                             sizeof(Wire), mn.c_str());
      WIN(map.handle.h, "mapping");
      map.wire = (Wire *)MapViewOfFile(map.handle.h, FILE_MAP_ALL_ACCESS, 0, 0,
                                       sizeof(Wire));
      WIN(map.wire, "mapping");
      Wire &w = *map.wire;
      memset(&w, 0, sizeof(w));
      w.version = 1;
      w.size = sizeof(w);
      w.pid = GetCurrentProcessId();
      w.colour = colour;
      DXGI_ADAPTER_DESC1 ad{};
      HR(adapter->GetDesc1(&ad), "adapter");
      w.luid = ad.AdapterLuid;
      Com<ID3D11Fence> fences[3];
      Com<ID3D11Texture2D> tex[7];
      Handle handles[10];
      for (int i = 0; i < 3; i++) {
        HR(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                           (void **)fences[i].out()),
           names[i]);
        HR(fences[i]->CreateSharedHandle(nullptr, GENERIC_ALL,
                                         resource_name(w.pid, i).c_str(),
                                         &handles[i].h),
           names[i]);
        w.handles[i] = (uintptr_t)handles[i].h;
      }
      for (int i = 0; i < 7; i++) {
        UINT bind = D3D11_BIND_SHADER_RESOURCE;
        if (i >= 2)
          bind |= D3D11_BIND_UNORDERED_ACCESS;
        if (i == 3 || i == 6)
          bind |= D3D11_BIND_RENDER_TARGET;
        D3D11_TEXTURE2D_DESC d = {8,
                                  8,
                                  1,
                                  1,
                                  format(i, colour),
                                  {1, 0},
                                  D3D11_USAGE_DEFAULT,
                                  bind,
                                  0,
                                  D3D11_RESOURCE_MISC_SHARED |
                                      D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
        HR(dev->CreateTexture2D(&d, nullptr, tex[i].out()), names[i + 3]);
        std::vector<unsigned char> data(8 * 8 * stride(i), input_bytes[i]);
        ctx->UpdateSubresource(tex[i].p, 0, nullptr, data.data(), 8 * stride(i),
                               0);
        Com<IDXGIResource1> r;
        HR(tex[i]->QueryInterface(__uuidof(IDXGIResource1), (void **)r.out()),
           names[i + 3]);
        HR(r->CreateSharedHandle(
               nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
               resource_name(w.pid, i + 3).c_str(), &handles[i + 3].h),
           names[i + 3]);
        w.handles[i + 3] = (uintptr_t)handles[i + 3].h;
      }
      HR(c4->Signal(fences[0].p, 1), "game-fence");
      ctx->Flush();
      if (!wcscmp(mode, L"invalid-handle"))
        w.handles[5] = 0xdeadbeef;
      if (!wcscmp(mode, L"invalid-wire"))
        w.version = 0;
      uint32_t source_pid = w.pid;
      Handle ready(CreateEventW(nullptr, TRUE, FALSE,
                                resource_name(source_pid, 101).c_str())),
          done(CreateEventW(nullptr, TRUE, FALSE,
                            resource_name(source_pid, 100).c_str()));
      WIN(ready.h, "ready-event");
      WIN(done.h, "done-event");
      if (!wcscmp(mode, L"missing-wire")) {
        UnmapViewOfFile(map.wire);
        map.wire = nullptr;
        CloseHandle(map.handle.h);
        map.handle.h = nullptr;
      }
      std::wstring cmd = L"\"" + std::wstring(consumer) + L"\" " +
                         std::to_wstring(source_pid) + L" " + mode;
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      PROCESS_INFORMATION pi{};
      WIN(CreateProcessW(consumer, cmd.data(), nullptr, nullptr, TRUE, 0,
                         nullptr, nullptr, &si, &pi),
          "consumer");
      Child process(pi.hProcess);
      Handle thread(pi.hThread);
      DWORD result;
      if (!wcscmp(mode, L"normal")) {
        HANDLE waits[] = {ready.h, process.h};
        result = WaitForMultipleObjects(2, waits, FALSE, 4 * timeout_ms);
        if (result != WAIT_OBJECT_0) {
          TerminateProcess(process.h, 124);
          WaitForSingleObject(process.h, timeout_ms);
          fail("consumer", "ready-event or early exit", result);
        }
      } else
        result = WaitForSingleObject(process.h, 4 * timeout_ms);
      if (result != WAIT_OBJECT_0) {
        TerminateProcess(process.h, 124);
        WaitForSingleObject(process.h, timeout_ms);
        fail("consumer", "process timeout", result);
      }
      DWORD exit = 0;
      if (wcscmp(mode, L"normal"))
        WIN(GetExitCodeProcess(process.h, &exit), "consumer");
      if (wcscmp(mode, L"normal")) {
        if (exit != 20)
          fail("negative test", "wrong failure or unexpected success", exit);
        fprintf(stderr, "PASS expected failure mode=%ls exit=%lu\n", mode,
                exit);
      } else {

        for (int i = 1; i < 3; i++) {
          wait11(fences[i].p, 1, names[i]);
          HR(c4->Wait(fences[i].p, 1), names[i]);
        }
        for (int i = 4; i < 6; i++) {
          D3D11_TEXTURE2D_DESC d{};
          tex[i]->GetDesc(&d);
          d.Usage = D3D11_USAGE_STAGING;
          d.BindFlags = 0;
          d.MiscFlags = 0;
          d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          Com<ID3D11Texture2D> stage;
          HR(dev->CreateTexture2D(&d, nullptr, stage.out()), names[i + 3]);
          ctx->CopyResource(stage.p, tex[i].p);
          ctx->Flush();
          D3D11_MAPPED_SUBRESOURCE m{};
          HRESULT r;
          DWORD start = GetTickCount();
          do {
            r = ctx->Map(stage.p, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
                         &m);
            if (r == DXGI_ERROR_WAS_STILL_DRAWING) {
              if (GetTickCount() - start > timeout_ms)
                fail(names[i + 3], "Map timeout", WAIT_TIMEOUT);
              Sleep(1);
            }
          } while (r == DXGI_ERROR_WAS_STILL_DRAWING);
          HR(r, names[i + 3]);
          verify((unsigned char *)m.pData, m.RowPitch, 4, output_bytes[i - 4],
                 names[i + 3]);
          ctx->Unmap(stage.p, 0);
        }
        WIN(SetEvent(done.h), "done-event");
        result = WaitForSingleObject(process.h, timeout_ms);
        if (result != WAIT_OBJECT_0) {
          TerminateProcess(process.h, 124);
          WaitForSingleObject(process.h, timeout_ms);
          fail("consumer", "exit timeout", result);
        }
        WIN(GetExitCodeProcess(process.h, &exit), "consumer");
        if (exit)
          fail("consumer", "exit status", exit);
      }
    }
    DWORD after = 0;
    WIN(GetProcessHandleCount(GetCurrentProcess(), &after), "producer");
    fprintf(stderr, "cycle=%d handles before=%lu after=%lu\n", cycle, before,
            after);
    // Wine/driver lazily opens process-lifetime handles on the first cycle.
    if (cycle > 0 && after > before)
      fail("producer", "handle count grew", after - before);
  }
  fprintf(stderr, "PASS probe mode=%ls cycles=%d\n", mode, cycles);
  return 0;
}
#else
static void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                       D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
  cl->ResourceBarrier(1, &b);
}
static int consumer(uint32_t pid, const wchar_t *mode) {
  Mapping map;
  map.handle.h =
      OpenFileMappingW(FILE_MAP_READ, FALSE, resource_name(pid, 99).c_str());
  if (!map.handle.h && !wcscmp(mode, L"missing-wire") &&
      GetLastError() == ERROR_FILE_NOT_FOUND) {
    fprintf(stderr,
            "EXPECTED resource=mapping API=OpenFileMappingW error=%lu\n",
            GetLastError());
    return 20;
  }
  WIN(map.handle.h, "mapping");
  map.wire =
      (Wire *)MapViewOfFile(map.handle.h, FILE_MAP_READ, 0, 0, sizeof(Wire));
  WIN(map.wire, "mapping");
  Wire w = *map.wire;
  if (!wcscmp(mode, L"invalid-wire") && w.version == 0) {
    fprintf(stderr, "EXPECTED resource=mapping API=validate-wire "
                    "error=ERROR_INVALID_DATA\n");
    return 20;
  }
  if (w.version != 1 || w.size != sizeof(Wire) || w.pid != pid)
    fail("mapping", "validate wire", ERROR_INVALID_DATA);
  Handle process(OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid));
  WIN(process.h, "producer");
  fprintf(stderr, "PASS OpenProcess(PROCESS_DUP_HANDLE) pid=%lu\n",
          (unsigned long)pid);
  Com<IDXGIFactory4> fac;
  HR(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)fac.out()),
     "factory");
  Com<IDXGIAdapter1> ad;
  HR(fac->EnumAdapterByLuid(w.luid, __uuidof(IDXGIAdapter1), (void **)ad.out()),
     "adapter");
  adapter_log(ad.p);
  Com<ID3D12Device> dev;
  HR(D3D12CreateDevice(ad.p, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                       (void **)dev.out()),
     "consumer");
  HANDLE baseline = nullptr;
  HRESULT named = dev->OpenSharedHandleByName(resource_name(pid, 0).c_str(),
                                              GENERIC_ALL, &baseline);
  fprintf(stderr,
          "BASELINE resource=game-fence API=OpenSharedHandleByName "
          "HRESULT=0x%08lx\n",
          (unsigned long)named);
  if (baseline)
    CloseHandle(baseline);
  Com<ID3D12Fence> fences[3];
  Com<ID3D12Resource> tex[7];
  for (int i = 0; i < 10; i++) {
    Handle dup;
    BOOL duplicated = DuplicateHandle(
        process.h, (HANDLE)(uintptr_t)w.handles[i], GetCurrentProcess(), &dup.h,
        0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!duplicated && i == 5 && !wcscmp(mode, L"invalid-handle") &&
        GetLastError() == ERROR_INVALID_HANDLE) {
      fprintf(stderr, "EXPECTED resource=%s API=DuplicateHandle error=%lu\n",
              names[i], GetLastError());
      return 20;
    }
    WIN(duplicated, names[i]);
    fprintf(stderr, "PASS DuplicateHandle resource=%s\n", names[i]);
    if (i < 3)
      HR(dev->OpenSharedHandle(dup.h, __uuidof(ID3D12Fence),
                               (void **)fences[i].out()),
         names[i]);
    else
      HR(dev->OpenSharedHandle(dup.h, __uuidof(ID3D12Resource),
                               (void **)tex[i - 3].out()),
         names[i]);
    fprintf(stderr, "PASS OpenSharedHandle resource=%s\n", names[i]);
    if (i == 8 && !wcscmp(mode, L"partial-import")) {
      fprintf(stderr, "EXPECTED resource=reactive API=injected-partial-import "
                      "error=ERROR_OPERATION_ABORTED\n");
      return 20;
    }
  }
  Com<ID3D12CommandQueue> q;
  D3D12_COMMAND_QUEUE_DESC qd{};
  HR(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                             (void **)q.out()),
     "queue");
  wait12(fences[0].p, 1, "game-fence");
  HR(q->Wait(fences[0].p, 1), "game-fence");
  Com<ID3D12CommandAllocator> alloc;
  HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                 __uuidof(ID3D12CommandAllocator),
                                 (void **)alloc.out()),
     "allocator");
  Com<ID3D12GraphicsCommandList> cl;
  HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr,
                            __uuidof(ID3D12GraphicsCommandList),
                            (void **)cl.out()),
     "list");
  Com<ID3D12Resource> readback[7], upload[2];
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[7]{};
  for (int i = 0; i < 7; i++) {
    auto desc = tex[i]->GetDesc();
    UINT64 size = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &footprints[i], nullptr, nullptr,
                               &size);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HR(dev->CreateCommittedResource(
           &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
           nullptr, __uuidof(ID3D12Resource), (void **)readback[i].out()),
       names[i + 3]);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = tex[i].p;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback[i].p;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprints[i];
    transition(cl.p, tex[i].p, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(cl.p, tex[i].p, D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    if (i == 4 || i == 5) {
      hp.Type = D3D12_HEAP_TYPE_UPLOAD;
      HR(dev->CreateCommittedResource(
             &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
             nullptr, __uuidof(ID3D12Resource), (void **)upload[i - 4].out()),
         names[i + 3]);
      void *p = nullptr;
      D3D12_RANGE empty{};
      HR(upload[i - 4]->Map(0, &empty, &p), names[i + 3]);
      memset(p, output_bytes[i - 4], (size_t)size);
      upload[i - 4]->Unmap(0, nullptr);
      src.pResource = upload[i - 4].p;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = footprints[i];
      dst.pResource = tex[i].p;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = 0;
      transition(cl.p, tex[i].p, D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_COPY_DEST);
      cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(cl.p, tex[i].p, D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_COMMON);
    }
  }
  HR(cl->Close(), "list");
  ID3D12CommandList *lists[] = {cl.p};
  q->ExecuteCommandLists(1, lists);
  HR(q->Signal(fences[1].p, 1), "sr-fence");
  HR(q->Signal(fences[2].p, 1), "present-fence");
  wait12(fences[2].p, 1, "present-fence");
  for (int i = 0; i < 7; i++) {
    void *p = nullptr;
    HR(readback[i]->Map(0, nullptr, &p), names[i + 3]);
    verify((unsigned char *)p + footprints[i].Offset,
           footprints[i].Footprint.RowPitch, stride(i), input_bytes[i],
           names[i + 3]);
    D3D12_RANGE empty{};
    readback[i]->Unmap(0, &empty);
  }
  fprintf(stderr, "PASS consumer GPU texture and fence roundtrip\n");
  Handle ready(
      OpenEventW(EVENT_MODIFY_STATE, FALSE, resource_name(pid, 101).c_str())),
      done(OpenEventW(SYNCHRONIZE, FALSE, resource_name(pid, 100).c_str()));
  WIN(ready.h, "ready-event");
  WIN(done.h, "done-event");
  WIN(SetEvent(ready.h), "ready-event");
  DWORD result = WaitForSingleObject(done.h, 2 * timeout_ms);
  if (result != WAIT_OBJECT_0)
    fail("producer", "return verification timeout", result);
  return 0;
}
#endif
int wmain(int argc, wchar_t **argv) {
  setvbuf(stderr, nullptr, _IONBF, 0);
  try {
#ifndef _WIN64
    if (argc < 2) {
      fprintf(
          stderr,
          "usage: probe-i686.exe consumer.exe [rgba|bgra] [cycles=3] "
          "[normal|invalid-handle|invalid-wire|missing-wire|partial-import]\n");
      return 2;
    }
    int cycles = argc > 3 ? _wtoi(argv[3]) : 3;
    if (cycles < 1 || cycles > 100)
      return 2;
    return producer(argv[1],
                    argc > 2 && !wcscmp(argv[2], L"bgra")
                        ? DXGI_FORMAT_B8G8R8A8_UNORM
                        : DXGI_FORMAT_R8G8B8A8_UNORM,
                    cycles, argc > 4 ? argv[4] : L"normal");
#else
    if (argc != 3)
      return 2;
    return consumer((uint32_t)wcstoul(argv[1], nullptr, 10), argv[2]);
#endif
  } catch (const std::exception &) {
    return 1;
  }
}
