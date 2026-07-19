#pragma once
#include "../common/ITracer.hpp"
#include <asm/ptrace.h>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <errno.h>
#include <linux/ptrace.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>


#include "Logger.hpp"

// todo 添加ptrace 硬件端点
// https://5ec1cff.github.io/my-blog/2024/06/24/android-arm64-hwbkpt/

// 各构架预定义
#if defined(__aarch64__) // 真机64位
#define pt_regs user_pt_regs
#define uregs regs
#define ARM_pc pc
#define ARM_sp sp
#define ARM_cpsr pstate
#define ARM_lr regs[30]
#define ARM_r0 regs[0]
#define BREAKPOINT_INSTR 0xD4200000 // BRK #0
// 这两个宏定义比较有意思 意思就是在 arm64下
// 强制 PTRACE_GETREGS 为 PTRACE_GETREGSET 这种
#define PTRACE_GETREGS PTRACE_GETREGSET
#define PTRACE_SETREGS PTRACE_SETREGSET

#elif defined(__x86_64__) // ？？未知架构
#define pt_regs user_regs_struct
#define eax rax
#define esp rsp
#define eip rip
#elif defined(__i386__) // 模拟器
#define pt_regs user_regs_struct
#endif

// 其余预定义


class AndroidTracer : public ITracer {
private:
  pid_t pid_;
  bool attached_;
  bool autoRestoreRegs_;

  // 检查进程状态
  bool IsProcessAlive() const {
    if (pid_ <= 0)
      return false;
    return kill(pid_, 0) == 0;
  }

public:
  AndroidTracer() : pid_(0), attached_(false), autoRestoreRegs_(true) {}

  bool Initialize(pid_t pid) override {
    pid_ = pid;
    return pid_ > 0;
  }

  bool Attach() override {
    if (pid_ <= 0)
      return false;
    if (attached_)
      return true;

    if (ptrace(PTRACE_ATTACH, pid_, nullptr, nullptr) == -1) {
      return false;
    }

    int status;
    // if (Wait(&status) != pid_ || !WIFSTOPPED(status)) {
    //   ptrace(PTRACE_DETACH, pid_, nullptr, nullptr);
    //   return false;
    // }
    LOGDF("[+] attach porcess success, pid:%d\n", pid_);
    waitpid(pid_, &status, WUNTRACED);


    attached_ = true;
    return true;
  }

  bool Detach() override {
    if (!attached_)
      return true;

    bool success = (ptrace(PTRACE_DETACH, pid_, nullptr, nullptr) != -1);
    if (success) {
      attached_ = false;
    }
    return success;
  }

  bool GetRegs(void *regs) override {
    if (!attached_ || !regs)
      return false;

    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(user_pt_regs);

    return ptrace(PTRACE_GETREGSET, pid_, NT_PRSTATUS, &iov) != -1;
  }

  uint64_t GetRegsRet(uint64_t reg_name) {
    if (!attached_)
      return 0;
    user_pt_regs regs;
    if (!GetRegs(&regs))
      return 0;
    return regs.uregs[reg_name];
  }

  uint64_t GetRegsRetX0() { return GetRegsRet(0); }

  bool SetRegs(void *regs) override {
    if (!attached_ || !regs)
      return false;

    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(user_pt_regs);

    return ptrace(PTRACE_SETREGSET, pid_, NT_PRSTATUS, &iov) != -1;
  }

  bool Continue() override {
    if (!attached_)
      return false;
    return ptrace(PTRACE_CONT, pid_, nullptr, nullptr) != -1;
  }

  bool Step() override {
    if (!attached_)
      return false;
    return ptrace(PTRACE_SINGLESTEP, pid_, nullptr, nullptr) != -1;
  }

  bool Wait(int *status) override {
    if (!attached_)
      return false;
    return waitpid(pid_, status, 0) == pid_;
  }

  bool Wait_opt(int *status, int options) {
    if (!attached_)
      return false;
    return waitpid(pid_, status, options) == pid_;
  }

  bool ReadMemory(uintptr_t address, void *buffer, size_t size) override {
    if (!attached_ || !buffer || !size)
      return false;

     long nReadCount = 0;
    long nRemainCount = 0;
    uint8_t *pCurSrcBuf = (uint8_t *)address;
    uint8_t *pCurDestBuf = (uint8_t *)buffer;
    long lTmpBuf = 0;
    long i = 0;

    nReadCount = size / sizeof(long);
    nRemainCount = size % sizeof(long);

    for (i = 0; i < nReadCount; i++) {
        errno = 0;
        lTmpBuf = ptrace(PTRACE_PEEKTEXT, pid_, pCurSrcBuf, 0);
        if (lTmpBuf == -1 && errno != 0) {
            return false;
        }
        memcpy(pCurDestBuf, (char *) (&lTmpBuf), sizeof(long));
        pCurSrcBuf += sizeof(long);
        pCurDestBuf += sizeof(long);
    }

    if (nRemainCount > 0) {
        errno = 0;
        lTmpBuf = ptrace(PTRACE_PEEKTEXT, pid_, pCurSrcBuf, 0);
        if (lTmpBuf == -1 && errno != 0) {
            return false;
        }
        memcpy(pCurDestBuf, (char *) (&lTmpBuf), nRemainCount);
    }

    return true;
  }

  bool WriteMemory(uintptr_t address, const void *buffer, size_t size) override {
    if (!attached_ || !buffer || !size)
      return false;

    long nWriteCount = 0;
    long nRemainCount = 0;
    const uint8_t *pCurSrcBuf = (const uint8_t *)buffer;
    uint8_t *pCurDestBuf =  (uint8_t *)address;
    long lTmpBuf = 0;
    long i = 0;

    nWriteCount = size / sizeof(long);
    nRemainCount = size % sizeof(long);

    // 先讲数据以sizeof(long)字节大小为单位写入到远程进程内存空间中
    for (i = 0; i < nWriteCount; i++){
        memcpy((void *)(&lTmpBuf), pCurSrcBuf, sizeof(long));
        if (ptrace(PTRACE_POKETEXT, pid_, (void *)pCurDestBuf, (void *)lTmpBuf) < 0){ // PTRACE_POKETEXT表示从远程内存空间写入一个sizeof(long)大小的数据
            LOGEF("[-] Write Remote Memory error, MemoryAddr:0x%lx, err:%s\n", (uintptr_t)pCurDestBuf, strerror(errno));
            return false;
        }
        pCurSrcBuf += sizeof(long);
        pCurDestBuf += sizeof(long);
    }
    // 将剩下的数据写入到远程进程内存空间中
    if (nRemainCount > 0){
        lTmpBuf = ptrace(PTRACE_PEEKTEXT, pid_, pCurDestBuf, NULL); //先取出原内存中的数据，然后将要写入的数据以单字节形式填充到低字节处
        memcpy((void *)(&lTmpBuf), pCurSrcBuf, nRemainCount);
        if (ptrace(PTRACE_POKETEXT, pid_, pCurDestBuf, lTmpBuf) < 0){
            LOGEF("[-] Write Remote Memory error, MemoryAddr:0x%lx, err:%s\n", (uintptr_t)pCurDestBuf, strerror(errno));
            return false;
        }
    }
    return true;
  }

  uintptr_t CallFunction(uintptr_t address, int nargs, ...) override {
    va_list args;
    va_start(args, nargs);
    uintptr_t result = CallFunctionV(address, nargs, args);
    va_end(args);
    return result;
  }

  uintptr_t CallFunctionV(uintptr_t address, int nargs, va_list args) override {
    if (!attached_ || !address)
      return 0;

    // 保存原始寄存器
    user_pt_regs original_regs;
    if (!GetRegs(&original_regs))
      return 0;

    // 设置参数寄存器
    user_pt_regs call_regs = original_regs;
    call_regs.pc = address;

    // ARM64 调用约定: x0-x7 用于传递参数
    for (int i = 0; i < nargs && i < 8; i++) {
      call_regs.regs[i] = va_arg(args, uint64_t);
    }

    // 设置新寄存器
    if (!SetRegs(&call_regs))
      return 0;

    // 执行函数
    if (!Continue())
      return 0;

    // 等待函数执行完成（带超时）
    int status;
    constexpr int kMaxWaitAttempts = 1000; // 防止无限挂起
    int waitAttempts = 0;
    while (Wait(&status)) {
      if (WIFSTOPPED(status) &&
          (WSTOPSIG(status) == SIGSEGV || WSTOPSIG(status) == SIGILL)) {
        break;
      }
      if (WIFEXITED(status))
        break;
      if (++waitAttempts >= kMaxWaitAttempts) {
        // 超时，恢复寄存器并中止
        SetRegs(&original_regs);
        return 0;
      }
      if (!Continue())
        break;
    }

    // 获取返回值
    user_pt_regs return_regs;
    if (!GetRegs(&return_regs))
      return 0;

    // 恢复原始寄存器
    if (autoRestoreRegs_) {
      SetRegs(&original_regs);
    }

    return return_regs.regs[0]; // x0 寄存器存储返回值
  }

};
