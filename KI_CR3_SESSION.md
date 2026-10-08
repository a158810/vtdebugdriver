# Persistent CR3-filtered Ki session

The user has exercised the earlier exception/context-update path in a VM. The new unload handshake remains experimental and has not been run in a VM. A successful build is not a runtime validation. Replace Host, Guest, and debugger binaries together and test only in a recoverable AMD VM with crash dumps and kernel debugging available.

## Event path

The debugger starts `IOCTL_LMV_GUEST_KI_SESSION` with the target PID. Guest uses `PsLookupProcessByProcessId` to retain an EPROCESS reference and reads the kernel CR3 directly from `EPROCESS + LMV_KPROCESS_DIRECTORY_TABLE_BASE_OFFSET` (currently `0x28`), masking it with `LMV_PAGE_ADDRESS_MASK`. It never switches address spaces with `KeStackAttachProcess`. The layout is configured for the supplied Windows 10 22H2 build 19045.6466 environment; verify the offsets against VM symbols before testing. Host masks CR3 with the CPU physical-address width, publishes the Ki entry and two Guest stubs, and Guest synchronizes the private DR3 configuration on each active processor, including processor groups. A partial arm attempts to stop the shared session and disarm every processor.

Session start/stop and controller cleanup are serialized by a signaled synchronization event, using only event wait/wake operations and without raising IRQL. Module queries, process lookup, CR3 reads, and processor synchronization run outside fast-mutex critical sections. The event-queue fast mutex remains limited to short state updates; it does not represent process attachment. Ki fallback scanning uses page-sized MmCopyMemory reads, including pattern overlap across page boundaries, rather than dereferencing every address in the kernel image. Unreadable portions are skipped, so fallback resolution can still fail.

Host keeps the private breakpoint armed after a hit. Matching CR3 and user PreviousMode select the event stub. Other hits select the pass stub. Guest rechecks process identity, PreviousMode, first chance, user frame addresses, IRQL, and IF before queueing. First-chance user exceptions wait in the original Guest thread, never in Host VMEXIT. The queue is bounded to 16 events and rejects same-thread reentry and overflow by passing the exception onward.

## Entry replay

Only a Ki entry beginning with `40 55` is accepted. The pass stub executes `push rbp` on the original Ki entry stack and asks Host through its final VMMCALL to resume at entry + 2. It does not jump back to the DR3 address or emulate a Guest stack write in Host. The event stub preserves GPRs, flags, and volatile XMM registers around its C handler. The handled path restores the entry state, then asks Host through the capture/commit VMMCALL pair to resume at Ki's caller without replaying the push.

## Old-project continue semantics

The GUI uses the Guest event wait, continue, get-context, and set-context IOCTLs instead of Windows debugging APIs. Each claimed event is bound to its sequence and consumer file object. User GPR/RIP/RSP/RFLAGS snapshots come from KTRAP_FRAME and KEXCEPTION_FRAME, not from the Ki entry registers. Context writes reject kernel RIP/RSP and changes to privileged flag bits. Segment registers and DR registers are not editable through this snapshot.

Normal continue consumes the exception, matching the original dbgsys hook behavior. The debugger must repair a faulting instruction's state or change RIP when necessary; blindly continuing can raise the same exception again. Detach and controller-handle cleanup cancel queued events so cancelled exceptions follow normal Windows dispatch instead of being consumed. No synthetic attach breakpoint, full-process suspension, async break, or module/thread lifecycle events are provided. Target-exit notification/work-item APIs are not used: target exit does not automatically detach. Explicitly detach or close the controller handle before attaching another target; the retained EPROCESS reference is released only after successful session stop. This remains a single-target session, not the old project's multi-process hash table.

## Lifetime restrictions

After the first persistent session registration, Host and Guest set DriverUnload to null. Ordinary detach now disables the session, cancels events, synchronizes disarm on all active CPUs, and waits up to approximately five seconds for Host's in-flight count to reach zero. Timeout preserves the unload prohibition and process reference and session state for retry. Cleanup drops a closing controller's file-object owner on failure so a new controller can retry drain without retaining a dangling file-object pointer. Detach and reattach remain supported. Unload requires the explicit terminal prepare-unload operation described below; successful preparation prohibits further attachment and experimental probes for this Host lifetime. Reload both drivers to use debugging again. Do not force unload, sleep/hibernate, hot-add CPUs, use a native kernel debugger's DR3, or invoke internal devirtualization CPUID during a session. This protocol is for the persistent Ki path, not certification of unrelated one-shot/NPT probes.

The existing one-shot probe and synthetic queue interfaces remain available only before the first persistent session attempt; they are blocked thereafter even following detach. CR3 is an initial address-space filter, not a general process identity mechanism; Guest performs the final identity check. Persistent concurrent Ki handling, native debug-register coexistence, Windows-build compatibility, and failure rollback require VM testing before normal use.

## Suggested VM checks

### Drain and unload commands

Normal debugger window closure (X, Esc, or the default dialog OK route) is non-terminal: it detaches, joins the event worker, and closes both device handles without sending PREPARE_UNLOAD or retiring either driver. Restarting the debugger can reattach while the drivers remain loaded. Detach or worker-join failures keep the window open for retry; OnDestroy remains fallback resource cleanup. Driver unload preparation is a separate explicit operation using Control --ki-prepare-unload after closing the debugger. Successful preparation retires the drivers and prohibits reattachment until both are reloaded. Guest and Host are not automatically unloaded; stop Guest first, then Host. Forced process termination, OS shutdown and crashes are not covered by this window-close callback.

Deploy matching newly built Host and Guest binaries. Old Host/Guest mixes are unsupported: the new persistent exit stubs require Host's VMMCALL handler. The registration command intentionally rejects an old Host before arming a new Guest session.

1. Complete or cancel the exception, detach in the GUI, and close the debugger so its event-wait IOCTL and device handles finish.
2. Run \x64\Debug\LMvtdriverGuestControl.exe --ki-drain (or the matching Release tool). A detached session must show inflight=0, armed=0, enabled flag clear. Before retirement flags normally contain REGISTERED (1).
3. Run LMvtdriverGuestControl.exe --ki-prepare-unload. It closes admission, repeats stop/disarm, waits for drain, retires the Guest registration in Host, and restores the Guest unload callback. Host remains non-unloadable until Guest's unload callback finishes cleanup and sends a final direct release CPUID; only then is Host's unload callback restored. It must report PASS, inflight=0, armed=0, flags=00000004 and matching CPU counts. An open debugger, active event request, remaining probe, disarm failure or drain timeout blocks preparation. Do not unload on FAIL.
4. Close every tool/device handle. Stop the Guest service first, then the Host service, using the actual service names used when installing the drivers. The tool does not itself unload either driver.
5. After successful preparation, reattachment is intentionally rejected. Unload/reload both drivers, not just Guest, for a new debug session. These steps have not been executed in a VM by Codex.

Host increments in-flight before redirecting either the event or pass path. The pass stub replays push rbp and exits via VMMCALL; Host selects Ki+2 and decrements the count. The handled path uses two exit VMMCALLs: Host captures RAX/RFLAGS and disables IF/TF for the single pop rax between them; the final VMMCALL restores RAX/RFLAGS and selects the kernel caller RIP before decrementing. No counter decrement leaves a later Guest driver instruction to run. The return target must be a high canonical address outside Guest's image, and RSP must match the captured RSP plus eight. Invalid handshakes fail closed with #GP, not a forced decrement. Unexpected stack faults, NMI interactions, concurrency and scheduling still require disposable-VM validation.

The standalone --ki-drain-selftest checks drain predicates and exit-image boundaries only. Existing relocation selftests do not execute the persistent assembly path or prove hardware unload safety.

1. Verify Host alone starts cleanly, then load matching Guest and debugger binaries. Enable a dump and retain the VM snapshot.
2. Attach by PID to a test process waiting for input. Unsupported Ki bytes, active probes, or missing virtualized CPUs must reject attachment rather than silently fall back to Windows debug APIs.
3. Trigger one user exception. Verify target PID/TID, user RIP/RSP, and repeatable context reads. Confirm the unrelated processes continue running.
4. Repair the divisor or set a verified recovery RIP, apply the context, and continue. Then trigger a second exception on the same CPU and another CPU to verify persistent interception.
5. Detach while stopped; verify normal Windows dispatch resumes. Repeat with closing the controller and with target exit. Test two target threads separately before attempting concurrency.
6. On any hang, mismatch, or bugcheck, preserve the dump and restore the snapshot. Do not force unload or repeat a crashing kernel experiment.
