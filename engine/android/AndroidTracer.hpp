#pragma once
#include "../common/ITracer.hpp"
#include <asm-generic/fcntl.h>
#include <asm-generic/mman-common.h>
#include <asm/ptrace.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/mman.h>
#include <linux/ptrace.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/system_properties.h>



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
#define CPSR_T_MASK (1u << 5)



// selinux状态
inline struct process_selinux{
    const char *selinux_mnt;
    int enforce;
} process_selinux = {nullptr, -1};




/**
 * @brief 处理各SELinux判断的初始化
 */
inline void handle_selinux_init(){ // 执行优先级 102 切记执行优先级越低 越先执行
    // code from AOSP
    char buf[BUFSIZ], *p;
    FILE *fp = nullptr;
    struct statfs stfbuf;
    int rc;
    char *bufp;
    int exists = 0;

    if (process_selinux.selinux_mnt){ // 如果selinux_state有值了 就终止下面的行为
        return;
    }

    /* We check to see if the preferred mount point for selinux file
	 * system has a selinuxfs. */
    do {
        rc = statfs("/sys/fs/selinux", &stfbuf);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        if ((uint32_t)stfbuf.f_type == (uint32_t)SELINUX_MAGIC) {
            process_selinux.selinux_mnt = strdup("/sys/fs/selinux"); // 为 selinux_mnt 赋值
            return;
        }
    }

    /* Drop back to detecting it the long way. */
    fp = fopen("/proc/filesystems", "r");
    if (!fp){
        return;
    }

    while ((bufp = fgets(buf, sizeof buf - 1, fp)) != nullptr) {
        if (strstr(buf, "selinuxfs")) {
            exists = 1;
            break;
        }
    }

    if (!exists){
        goto out;
    }

    fclose(fp);

    /* At this point, the usual spot doesn't have an selinuxfs so
	 * we look around for it */
    fp = fopen("/proc/mounts", "r");
    if (!fp){
        goto out;
    }

    while ((bufp = fgets(buf, sizeof buf - 1, fp)) != nullptr) {
        char *tmp;
        p = strchr(buf, ' ');
        if (!p){
            goto out;
        }
        p++;
        tmp = strchr(p, ' ');
        if (!tmp){
            goto out;
        }
        if (!strncmp(tmp + 1, "selinuxfs ", 10)) {
            *tmp = '\0';
            break;
        }
    }

    /* If we found something, dup it */
    if (bufp){
        process_selinux.selinux_mnt = strdup(p);
    }

    out:
    if (fp){
        fclose(fp);
    }

    return;
}

/**
 *
 * @param value SELinux的状态值 0为宽容模式Permissive 1为严格模式Enforcing
 * @return
 */
inline bool set_selinux_state(int value) {
    int fd;
    char path[PATH_MAX];
    char buf[20];

    if (!process_selinux.selinux_mnt) {
        errno = ENOENT;
        return false;
    }

    snprintf(path, sizeof path, "%s/enforce", process_selinux.selinux_mnt);
    fd = open(path, O_RDWR);
    if (fd < 0)
        return false;

    snprintf(buf, sizeof buf, "%d", (int)value);
    int ret = write(fd, buf, strlen(buf));
    close(fd);
    return ret >= 0;
}


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

  /**
   * @brief 使用ptrace远程call函数
   *
   * @param pid pid表示远程进程的ID
   * @param ExecuteAddr ExecuteAddr为远程进程函数的地址
   * @param parameters parameters为函数参数的地址
   * @param num_params regs为远程进程call函数前的寄存器环境
   * @param regs
   * @return 返回0表示call函数成功，返回-1表示失败
   */
  int ptrace_call(pid_t pid, uintptr_t ExecuteAddr, long *parameters,
                  long num_params, struct pt_regs *regs,
                  uintptr_t return_addr) {

    int num_param_registers = 8;

    int i = 0;
    // ARM处理器，函数传递参数，将前四个参数放到r0-r3，剩下的参数压入栈中
    for (i = 0; i < num_params && i < num_param_registers; i++) {
      regs->uregs[i] = parameters[i];
    }

    if (i < num_params) {
      regs->ARM_sp -= (num_params - i) *
                      sizeof(long); // 分配栈空间，栈的方向是从高地址到低地址
      if (!WriteMemory(regs->ARM_sp, &parameters[i],
                      (num_params - i) * sizeof(long)))
        return -1;
    }

    regs->ARM_pc = ExecuteAddr; // 设置ARM_pc寄存器为需要调用的函数地址
    // 与BX跳转指令类似，判断跳转的地址位[0]是否为1，如果为1，则将CPST寄存器的标志T置位，解释为Thumb代码
    // 若为0，则将CPSR寄存器的标志T复位，解释为ARM代码
    if (regs->ARM_pc & 1) {
      /* thumb */
      regs->ARM_pc &= (~1u);
      regs->ARM_cpsr |= CPSR_T_MASK;
    } else {
      /* arm */
      regs->ARM_cpsr &= ~CPSR_T_MASK;
    }

    regs->ARM_lr = 0;

    // Android 7.0以上修正lr为libc.so的起始地址 getprop获取ro.build.version.sdk
    uintptr_t lr_val = 0;
    char sdk_ver[32];
    memset(sdk_ver, 0, sizeof(sdk_ver));
    __system_property_get("ro.build.version.sdk", sdk_ver);
    //    printf("ro.build.version.sdk: %s", sdk_ver);
    if (atoi(sdk_ver) <= 23) {
      lr_val = 0;
    } else { // Android 7.0
      uintptr_t start_ptr = return_addr;
      lr_val = start_ptr;
    }
    regs->ARM_lr = lr_val;
    if (!SetRegs(regs) || !Continue()) { 
      LOGDF("[-] ptrace set regs or continue error, pid:%d", pid);
      return -1;
    }

    int stat = 0;
    // 对于使用ptrace_cont运行的子进程，它会在3种情况下进入暂停状态：①下一次系统调用；②子进程退出；③子进程的执行发生错误。
    // 参数WUNTRACED表示当进程进入暂停状态后，立即返回
    // 将ARM_lr（存放返回地址）设置为0，会导致子进程执行发生错误，则子进程进入暂停状态
    waitpid(pid, &stat, WUNTRACED);

    // 判断是否成功执行函数
    LOGDF("[+] ptrace call ret status is %d", stat);
    while (true) {
      if ((stat & 0xFF) != 0x7f) {
        if (!Continue()) {
          LOGD("[-] ptrace call error");
          return -1;
        }
        waitpid(pid, &stat, WUNTRACED);
        break;
      } else {
        // 如果等于7f 说明程序运行发生错误
        if (WSTOPSIG(stat) == SIGSEGV) {
          if (!GetRegs(regs)) {
            LOGD("[-] After call getregs error");
            return -1;
          }
          LOGD("[-] child process is SIGSEGV");
          if (static_cast<uintptr_t>(regs->pc) != return_addr) {
            LOGDF("wrong return addr %p", (void *) regs->pc);
            return 0;
          }
          return regs->pc;
        }
      }
    }

    // 获取远程进程的寄存器值，方便获取返回值
    if (!GetRegs(regs)) {
      LOGD("[-] After call getregs error");
      return -1;
    }

    return 0;
  }

  bool InjectSo(const char *so_path, uint64_t libc_addrr, uint64_t mmap_addr,
                uint64_t dlopen_addr, uint64_t dlsym_addr = 0,
                const char *FunctionName = nullptr,uint64_t dlerror_addr = 0) {
    if ( !so_path || !Attach())
      return false;
    LOGDF("start inject %s",so_path);
handle_selinux_init();
set_selinux_state(0);
LOGD("set_selinux_state(0);");

    // RAII guard: 确保 SELinux 在任何退出路径都恢复
    auto selinux_guard = [&]() {
        set_selinux_state(1);
        LOGD("set_selinux_state(1);");
    };

    struct pt_regs CurrentRegs, OriginalRegs;
    if (!GetRegs(&CurrentRegs)) {
      selinux_guard();
      return false;
    }
    memcpy(&OriginalRegs, &CurrentRegs, sizeof(struct pt_regs));
     LOGD("start mmap");
    // 申请参数内存
    long parameters[6];
    parameters[0] = NULL;   // 设置为NULL表示让系统自动选择分配内存的地址
    parameters[1] = 0x1000; // 映射内存的大小
    parameters[2] = PROT_READ | PROT_WRITE; // 表示映射内存区域 可读|可写|可执行
    parameters[3] = MAP_ANONYMOUS | MAP_PRIVATE; // 建立匿名映射
    parameters[4] = -1; //  若需要映射文件到内存中，则为文件的fd
    parameters[5] = 0;  // 文件映射偏移量

    // 调用远程进程的mmap函数 建立远程进程的内存映射
    // 在目标进程中为libxxx.so分配内存
    if (ptrace_call(pid_, (uintptr_t)mmap_addr, parameters, 6, &CurrentRegs,
                    libc_addrr) == -1) {
      LOGDF("[-] Call Remote mmap Func Failed, err:%s", strerror(errno));
      SetRegs(&OriginalRegs);
      selinux_guard();
      return false;
    }

    // 获取mmap函数执行后的返回值，也就是内存映射的起始地址
    // 从寄存器中获取mmap函数的返回值 即申请的内存首地址
    auto RemoteMapMemoryAddr = (uintptr_t)CurrentRegs.uregs[0];
    LOGDF("[+] Remote Process Map Memory Addr:0x%lx", RemoteMapMemoryAddr);

    // 失败时清理 mmap 内存的 lambda
    auto cleanup_mmap = [&]() {
        // 调用远程 munmap 释放内存（复用 mmap_addr 附近的 munmap）
        // 简化处理：恢复寄存器即可，mmap 的内存在进程退出时会自动释放
        SetRegs(&OriginalRegs);
        selinux_guard();
    };

    // 将要加载的so库路径写入到远程进程内存空间中
    if (!WriteMemory(RemoteMapMemoryAddr, (void *)so_path,
                     strlen(so_path) + 1)) {
      LOGDF("[-] Write LibPath:%s to RemoteProcess error", so_path);
      cleanup_mmap();
      return false;
    }
    // 设置dlopen的参数,返回值为模块加载的地址
    // void *dlopen(const char *filename, int flag);
    parameters[0] = (uintptr_t)RemoteMapMemoryAddr; // 写入的libPath
    parameters[1] = RTLD_NOW; // dlopen的标识 不能使用RTLD_GLOBAL
                              // ,会导致无法dlclose 无法关闭so库

    // 执行dlopen 载入so
    if (ptrace_call(pid_, (uintptr_t)dlopen_addr, parameters, 2, &CurrentRegs,
                    libc_addrr) == -1) {
      LOGD("[-] Call Remote dlopen Func Failed");
      cleanup_mmap();
      return false;
    }

    // RemoteModuleAddr dlopen 的返回值 即加载的so库的地址
    //GetRegs(&CurrentRegs);
    void *RemoteModuleAddr = (void *)CurrentRegs.uregs[0];
    LOGDF("[+] ptrace_call dlopen success, Remote Process load module Addr:0x%lx",(long) RemoteModuleAddr);

    // dlopen 错误
    if ((long)RemoteModuleAddr == 0x0) {
      LOGD("[-] dlopen error");
      if (dlerror_addr != 0){
        if (ptrace_call(pid_, (uintptr_t) dlerror_addr, parameters, 0, &CurrentRegs,libc_addrr) == -1) {
          LOGD("[-] Call Remote dlerror Func Failed");
          cleanup_mmap();
          return false;
        }

      char *Error = (char *) CurrentRegs.uregs[0];
      char LocalErrorInfo[1024] = {0};
      ReadMemory((uintptr_t)Error, LocalErrorInfo, 1024);
      LOGDF("[-] dlopen error:%s", LocalErrorInfo);
      cleanup_mmap();
      return false;
      }
      cleanup_mmap();
      return false;
    }

    // 关于函数执行的问题 dlopen 会默认执行init段里面的函数
    /*
    __attribute__((constructor))
    void init_func() {
        // 这会在 so 加载时自动调用
    }

    dlopen() + RTLD_NOW	保证立即执行 .init_array
    dlopen() + RTLD_LAZY	同样会执行 init，但符号解析可能延迟
    */

    // 判断是否传入symbols
    if (dlsym_addr != 0 && FunctionName != nullptr) {
      LOGDF("[+] func symbols is %s", FunctionName);
      // 传入了函数的symbols
      LOGD("[+] Have func !!");
      // 将so库中需要调用的函数名称写入到远程进程内存空间中
      if (!WriteMemory(RemoteMapMemoryAddr + strlen(so_path) + 2,
                       (void *)FunctionName, strlen(FunctionName) + 1)) {
        LOGDF("[-] Write FunctionName:%s to RemoteProcess error", FunctionName);
        cleanup_mmap();
        return false;
      }

      // 设置dlsym的参数，返回值为远程进程内函数的地址 调用XXX功能
      // void *dlsym(void *handle, const char *symbol);
      parameters[0] = (uintptr_t)RemoteModuleAddr;
      parameters[1] =
          (uintptr_t)((uint8_t *)RemoteMapMemoryAddr + strlen(so_path) + 2);
      // 调用dlsym
      if (ptrace_call(pid_, (uintptr_t)dlsym_addr, parameters, 2, &CurrentRegs,
                      libc_addrr) == -1) {
        LOGDF("[-] Call Remote dlsym Func %s Failed", FunctionName);
        cleanup_mmap();
        return false;
      }

      // RemoteModuleFuncAddr为远程进程空间内获取的函数地址
      void *RemoteModuleFuncAddr = (void *)CurrentRegs.uregs[0];
      LOGDF("[+] ptrace_call dlsym success, Remote Process ModuleFunc Addr:0x%lx",(uintptr_t)RemoteModuleFuncAddr);

      // 调用远程进程到某功能 不支持参数传递 ！！
      if (ptrace_call(pid_, (uintptr_t)RemoteModuleFuncAddr, parameters, 0,
                      &CurrentRegs, libc_addrr) == -1) {
        LOGD("[-] Call Remote injected Func Failed");
        cleanup_mmap();
        return false;
      }
    }

    if (!SetRegs(&OriginalRegs)) {
      selinux_guard();
      return false;
    }

     LOGD("[+] Recover Regs Success\n");

selinux_guard();
    //detch
    Detach();

    return true;
  }

  uint64_t CallRmmap(uint64_t libc_addrr, uint64_t mmap_addr, int prot,
                     int flag, size_t size) {
    if (!mmap_addr)
      return 0;

    if (Attach()) {
      struct pt_regs CurrentRegs, OriginalRegs;
      if (!GetRegs(&CurrentRegs))
        return false;
      memcpy(&OriginalRegs, &CurrentRegs, sizeof(struct pt_regs));
      // mmap(void * _Nullable addr, size_t size, int prot, int flags, int fd,
      // off_t offset)
      //  申请参数内存
      long parameters[6];
      parameters[0] = NULL;   // 设置为NULL表示让系统自动选择分配内存的地址
      parameters[1] = size; // 映射内存的大小
      parameters[2] = prot; // 表示映射内存区域 可读|可写|可执行
      parameters[3] = flag; // 建立匿名映射
      parameters[4] = -1; //  若需要映射文件到内存中，则为文件的fd
      parameters[5] = 0;  // 文件映射偏移量

      // 调用远程进程的mmap函数 建立远程进程的内存映射
      // 在目标进程中为libxxx.so分配内存
      if (ptrace_call(pid_, (uintptr_t)mmap_addr, parameters, 6, &CurrentRegs,
                      libc_addrr) == -1) {
        LOGDF("[-] Call Remote mmap Func Failed, err:%s", strerror(errno));
        return false;
      }

      // 获取mmap函数执行后的返回值，也就是内存映射的起始地址
      // 从寄存器中获取mmap函数的返回值 即申请的内存首地址
      auto RemoteMapMemoryAddr = (uintptr_t)CurrentRegs.uregs[0];
      LOGDF("[+] Remote Process Map Memory Addr:0x%lx", RemoteMapMemoryAddr);
      SetRegs(&OriginalRegs);
    

     LOGD("[+] Recover Regs Success\n");

      Detach();
      return RemoteMapMemoryAddr;
    };

    return 0;
  }

    bool CallRmunmap(uint64_t libc_addrr, uint64_t unmmap_addr,uint64_t addr, size_t size) {
    if (!unmmap_addr)
      return 0;

    if (Attach()) {
      struct pt_regs CurrentRegs, OriginalRegs;
      if (!GetRegs(&CurrentRegs))
        return false;
      memcpy(&OriginalRegs, &CurrentRegs, sizeof(struct pt_regs));
      //munmap(void * _Nonnull addr, size_t size)
      //  申请参数内存
      long parameters[2];
      parameters[0] = addr;   // 分配内存的地址
      parameters[1] = size; // 映射内存的大小


      // 调用远程进程的mmap函数 建立远程进程的内存映射
      // 在目标进程中为libxxx.so分配内存
      if (ptrace_call(pid_, (uintptr_t)unmmap_addr, parameters, 2, &CurrentRegs,
                      libc_addrr) == -1) {
        LOGDF("[-] Call Remote mmap Func Failed, err:%s", strerror(errno));
        return false;
      }

      int ret = (uintptr_t)CurrentRegs.uregs[0];
      LOGDF("[+] Remote Process unMap Memory ret %d", ret);
    SetRegs(&OriginalRegs);
    
     LOGD("[+] Recover Regs Success\n");
      Detach();
      return ret==0;
    };

    return 0;
  }
};