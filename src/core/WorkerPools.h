#pragma once

#include <functional>

class QThreadPool;

// Thread pools for work that touches the filesystem, and the one safe way for
// such work to report back to the GUI thread.
//
// Why not QThreadPool::globalInstance(): a stat or read on a hung NFS/sshfs
// mount (or a device yanked mid-read) blocks its thread until the mount
// answers, possibly forever. On the global pool a few such paths starved every
// other QtConcurrent user in the process.
//
// Why these pools are never destroyed: ~QThreadPool waits for every running
// task, so owning one in a widget (or deleting one at exit) turned a stuck
// thread into a frozen window. They are allocated once and deliberately
// leaked; tasks must therefore never capture a raw `this` — they capture
// shared state (std::shared_ptr) and post results through postToGui().
namespace WorkerPools {

// Call once from main(), right after the QApplication is constructed. Arms
// the aboutToQuit hook that makes postToGui() stop posting.
void installShutdownGuard();

// Short filesystem work: directory revalidation, MIME classification, folder
// preview listings, image decoding. Sized above the core count because these
// threads spend their time blocked in syscalls, not computing — a stuck mount
// ties up some of them without idling the rest.
QThreadPool *io();

// Recursive name search. Bounded separately so a big search cannot crowd out
// previews, and previews cannot slow a search.
QThreadPool *search();

// Poppler work. A Poppler::Document must not render two pages at once; each
// PdfPreview holds a lock per document for that, so this pool only bounds how
// many documents load or render concurrently.
QThreadPool *pdf();

// Queues `fn` onto the GUI thread. Safe to call from any worker at any time:
// once the application has started shutting down the call is dropped and
// returns false, instead of posting to a QCoreApplication that is about to be
// (or already has been) destroyed. `fn` runs on the GUI thread, so that is
// where it must check whether its receiver still exists (QPointer).
bool postToGui(std::function<void()> fn);

} // namespace WorkerPools
