# Release Checklist

This checklist tracks the work required before publishing version `0.1.0`.
An item is complete only after its implementation, documentation, and relevant
tests have passed on the release candidate.

## 1. Driver lifecycle and concurrency

- [ ] Fix safe device removal while files are open.
- [ ] Reject new operations after device removal begins.
- [ ] Stop the RX monitor before disabling or releasing hardware resources.
- [ ] Terminate active DMA before freeing its buffer or channel.
- [ ] Ensure DMA buffers and MMIO resources are released exactly once.
- [ ] Make every probe failure path disable BSC DMA and the BSC peripheral.
- [ ] Unregister every successfully created interface during probe rollback.
- [ ] Fix the race between the last writer closing and a new writer opening.
- [ ] Serialize response replacement without erasing a newer writer's data.
- [ ] Make nonblocking writes return `EAGAIN` when TX setup is busy.
- [ ] Make `poll()` writable state match whether a write can start immediately.
- [ ] Wake writable poll waiters whenever TX setup becomes available.
- [ ] Test repeated module load and unload.
- [ ] Test repeated Device Tree bind and unbind.
- [ ] Test forced probe failures at each resource-acquisition stage.
- [ ] Test concurrent opens, writes, closes, and polling.

## 2. Per-device state

- [ ] Move MMIO, DMA, buffers, locks, counters, wait queues, and worker state
  from global variables into one device structure.
- [ ] Store and retrieve the structure through platform-device and file state.
- [ ] Remove assumptions that only one matching Device Tree node can exist.
- [ ] Confirm debugfs entries address the correct device instance.

## 3. Device Tree and pin control

- [ ] Add the required BCM2711 BSC pinctrl configuration to the overlay.
- [ ] Document the physical CM4 pins and carrier-board wiring.
- [ ] Confirm that conflicting pin users fail clearly.
- [ ] Validate the overlay on Raspberry Pi OS Bookworm and Ubuntu.

## 4. Userspace ABI

- [ ] Document `open()`, `read()`, `write()`, `poll()`, and close behavior.
- [ ] Document blocking and nonblocking behavior and all expected errors.
- [ ] Document response replacement and DMA completion semantics.
- [ ] Document RX overflow behavior and the SPI direction bytes.
- [x] Remove the obsolete fixed-size TX software ring.
- [x] Allocate the TX DMA buffer according to each write size.
- [ ] Define practical DMA allocation and descriptor failure behavior.
- [ ] Decide which debugfs files are stable diagnostics and which are internal.
- [ ] Publish the frozen ABI as a versioned document.

## 5. Test utility

- [ ] Add a small C utility that can open the character device.
- [ ] Support blocking and nonblocking reads.
- [ ] Support writes of binary and text data.
- [ ] Demonstrate `poll()` for readable and writable events.
- [ ] Report kernel errors clearly.
- [ ] Include repeatable loopback or master/slave test instructions.

## 6. Platform testing

- [x] Record existing testing with Raspberry Pi OS Bookworm.
- [x] Record existing testing with Ubuntu, ROS 2, and Python applications.
- [ ] Test the final release candidate on a clean Raspberry Pi OS image.
- [ ] Test the final release candidate on a clean Ubuntu image.
- [ ] Test install, reboot, data transfer, reinstall, upgrade, and uninstall.
- [ ] Record kernel, firmware, board revision, SPI mode, and clock rate.
- [ ] Test small, large, interrupted, and retried transfers.

## 7. Packaging and upgrades

- [x] Check the running kernel against the next-boot kernel during install.
- [x] Validate module version and kernel vermagic before installation.
- [x] Stage module and overlay replacements and roll back failed installs.
- [x] Record installed version, kernel, and artifact checksums.
- [ ] Add DKMS only after the driver lifecycle and ABI are stable.
- [ ] Test automatic rebuilds across at least two kernel upgrades.
- [ ] Add and test a udev rule for non-root access.

## 8. Version `0.1.0` release

- [ ] Resolve all release-blocking code-review findings.
- [ ] Run kernel style and static-analysis checks.
- [ ] Build without driver-source warnings on supported kernels.
- [ ] Verify README, installation guide, ABI document, and examples.
- [ ] Confirm the repository contains no generated files or credentials.
- [ ] Commit the release candidate and create an annotated `v0.1.0` tag.
- [ ] Publish source archives and release notes.

