# svmtest

`svmtest.sys` is an IOCTL-triggered standalone SVM test driver. It prepares one local VMCB and executes a real `VMRUN` only after an explicit control request. The basic test does not require `LMvtdriver.sys` or another parent hypervisor.

## Test modes

- `svmtestctl.exe run 1` (or `cpuid 1`) performs one standalone CPUID VMEXIT round trip. L2 executes the designated CPUID leaf, `svmtest.sys` supplies a fixed EAX value and resumes L2 from the VMCB-reported NRIP, then L2 reaches its completion VMMCALL.
- The broader smoke IOCTL is reserved for later work and is not part of the basic acceptance path.
- `capabilities`, `result`, and `reset` do not execute `VMRUN`. A successful capability query reports exposed hardware support only; it does not prove that the VMRUN round trip works.

The test protocol is version 3. `svmtest_shared.h` defines the IOCTLs, capability flags, and result structures.

## Requirements and safety

- Windows x64 on AMD SVM hardware, with SVM and NRIP-save exposed, a usable ASID, and `VM_CR.SVMDIS` clear. If Windows itself runs in a VM, its host must expose nested SVM.
- Load only `svmtest.sys` for the first test. The test pins itself to one processor, prepares `EFER.SVME`, `VM_HSAVE_PA`, a local VMCB and permission maps, then executes the guest on that processor.
- The old L0/nested capability bits remain in the protocol for compatibility, but they do not gate the standalone CPUID test. The run is rejected with `STATUS_DEVICE_BUSY` if `EFER.SVME` is already set, so the test will not overwrite another VMM's `VM_HSAVE_PA`.
- The L2 instruction and exception-handler labels execute from the driver's executable code section; VMCB, stack, IDT, and test data use the per-run allocated buffers.
- Run the control utility elevated. The device ACL allows only LocalSystem and Administrators.
- Use a disposable VM snapshot with kernel debugging and crash recovery configured. A successful build does not prove that the target processor accepts the guest state at runtime.
- Test buffers are freed after each normal or handled-failure run. A successful build does not prove SVM execution works on the target machine.

## Example

In an elevated command prompt in the disposable AMD VM, load only `svmtest.sys`:

```text
sc.exe create svmtest type= kernel binPath= "C:\test\svmtest.sys"
sc.exe start svmtest
svmtestctl.exe capabilities
```

Before running, confirm `svm_ready=1` and `svme_current=0`.

Then run the minimal test:

```text
svmtestctl.exe run 1
svmtestctl.exe result
```

A successful `run 1` reports `exit_eax=0x53564d54`, `expected_eax=0x4c305445`, and `observed_eax=0x4c305445`, with one CPUID exit and one completion VMMCALL.

`run` accepts 1-8 iterations. Validate `run 1`, `result`, and then `run 8` before extending the driver.

Stop `svmtest.sys` after the test:

```text
sc.exe stop svmtest
sc.exe delete svmtest
```
