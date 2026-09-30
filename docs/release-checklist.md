# Release Checklist

This checklist tracks the work required before publishing version `0.1.0`.
An item is complete only after its implementation, documentation, and relevant
tests have passed on the release candidate.

## 1. Driver lifecycle and concurrency

- [ ] Fix safe device removal while files are open.
- [x] Reject new operations after device removal begins.
- [x] Stop the RX monitor before disabling or releasing hardware resources.
- [x] Terminate active DMA before freeing its buffer or channel.
- [x] Ensure DMA buffers and MMIO resources are released exactly once.
- [x] Make every probe failure path disable BSC DMA and the BSC peripheral.
- [x] Unregister every successfully created interface during probe rollback.
- [x] Fix the race between the last writer closing and a new writer opening.
- [x] Serialize response replacement without erasing a newer writer's data.
- [x] Make nonblocking writes return `EAGAIN` when TX setup is busy.
- [x] Make `poll()` writable state match whether a write can start immediately.
- [x] Wake writable poll waiters whenever TX setup becomes available.
- [ ] Test repeated module load and unload.
- [ ] Test repeated Device Tree bind and unbind.
- [ ] Test forced probe failures at each resource-acquisition stage.
- [ ] Test concurrent opens, writes, closes, and polling.

## 2. Per-device state

- [x] Move MMIO, DMA, buffers, locks, counters, wait queues, and worker state
  from global variables into one device structure.
- [x] Store and retrieve the structure through platform-device and file state.
- [x] Remove assumptions that only one matching Device Tree node can exist.
- [x] Confirm debugfs entries address the correct device instance.

## 3. Device Tree and pin control

- [x] Add the required BCM2711 BSC pinctrl configuration to the overlay.
- [x] Document the physical CM4 pins and carrier-board wiring.
- [ ] Confirm that conflicting pin users fail clearly.
- [ ] Validate the overlay on Raspberry Pi OS Bookworm and Ubuntu.

## 4. Userspace ABI

- [x] Document `open()`, `read()`, `write()`, `poll()`, and close behavior.
- [x] Document blocking and nonblocking behavior and all expected errors.
- [x] Document response replacement and DMA completion semantics.
- [x] Document RX overflow behavior and the SPI direction bytes.
- [x] Keep `/dev/cm4_spi_slave` for compatibility with existing ROS 2 and
  Python applications.
- [x] Avoid a compatibility alias by keeping the existing device name.
- [x] Remove the obsolete fixed-size TX software ring.
- [x] Allocate the TX DMA buffer according to each write size.
- [x] Define practical DMA allocation and descriptor failure behavior.
- [x] Keep all debugfs files outside the stable userspace ABI.
- [x] Publish the frozen ABI as a versioned document.

## 5. Test utility

- [x] Add a small C utility that can open the character device.
- [x] Support blocking and nonblocking reads.
- [x] Support writes of binary and text data.
- [x] Demonstrate `poll()` for readable and writable events.
- [x] Report kernel errors clearly.
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
- [x] Refuse scripted uninstall while `/dev/cm4_spi_slave` is open.
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
