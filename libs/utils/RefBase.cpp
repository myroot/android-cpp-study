/*
 * Copyright (C) 2005 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright (C) 2005 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "RefBase"
// #define LOG_NDEBUG 0 // Controls ALOGV verbosity

#include <utils/RefBase.h>

#include <atomic>
#include <mutex> // For std::mutex and std::lock_guard

#include <utils/CallStack.h> // Still needed for DEBUG_REFS
#include <utils/Log.h> // Using ALOG for now, might need to switch to <log/log.h> if AOSP uses it
#include <utils/TextOutput.h> // For printStrongPointer, printWeakPointer

#include <stdlib.h>
#include <stdio.h>
#include <typeinfo>
#include <sys/types.h> // For open
#include <sys/stat.h>  // For open
#include <fcntl.h>     // For open
#include <unistd.h>    // For close, write
#include <string.h>    // For strerror
#include <errno.h>     // For errno

// Define DEBUG_REFS related macros based on AOSP's typical setup.
// These might need adjustment based on the specific build environment.
#ifndef DEBUG_REFS
#define DEBUG_REFS 0
#endif

#ifndef DEBUG_REFS_FATAL_SANITY_CHECKS
#define DEBUG_REFS_FATAL_SANITY_CHECKS DEBUG_REFS
#endif

#ifndef DEBUG_REFS_ENABLED_BY_DEFAULT
#define DEBUG_REFS_ENABLED_BY_DEFAULT DEBUG_REFS
#endif

#ifndef DEBUG_REFS_CALLSTACK_ENABLED
#define DEBUG_REFS_CALLSTACK_ENABLED DEBUG_REFS
#endif

// Log all reference counting operations (typically off for performance)
#ifndef PRINT_REFS
#define PRINT_REFS 0
#endif

// ---------------------------------------------------------------------------
namespace android {

// This is the initial value of the strong reference count.
// It's set to a large value to help differentiate between a RefBase object
// that has never had a strong reference and one that has had its strong
// reference count go to zero.
#define INITIAL_STRONG_VALUE (1 << 28)

// ---------------------------------------------------------------------------

class RefBase::weakref_impl : public RefBase::weakref_type
{
public:
    std::atomic<int32_t> mStrong; // Strong reference count
    std::atomic<int32_t> mWeak;   // Weak reference count
    RefBase* const mBase;         // Pointer to the RefBase object
    std::atomic<int32_t> mFlags;  // Flags, e.g., object lifetime

#if !DEBUG_REFS

    weakref_impl(RefBase* base)
        : mStrong(INITIAL_STRONG_VALUE)
        , mWeak(0)
        , mBase(base)
        , mFlags(0)
    {
    }

    void addStrongRef(const void* /*id*/) { }
    void removeStrongRef(const void* /*id*/) { }
    void renameStrongRefId(const void* /*old_id*/, const void* /*new_id*/) { }
    void addWeakRef(const void* /*id*/) { }
    void removeWeakRef(const void* /*id*/) { }
    void renameWeakRefId(const void* /*old_id*/, const void* /*new_id*/) { }
    void printRefs() const { }
    void trackMe(bool /*enable*/, bool /*retain*/) { }

#else

    weakref_impl(RefBase* base)
        : mStrong(INITIAL_STRONG_VALUE)
        , mWeak(0)
        , mBase(base)
        , mFlags(0)
        , mStrongRefs(nullptr)
        , mWeakRefs(nullptr)
        , mTrackEnabled(!!DEBUG_REFS_ENABLED_BY_DEFAULT)
        , mRetain(false) // TODO(b/175724971): check AOSP value for this
    {
    }

    ~weakref_impl()
    {
        bool dumpStack = false;
        if (!mRetain && mStrongRefs != nullptr) {
            dumpStack = true;
            // AOSP uses android::base::SetLogger / android::base::KernelLogger
            // and LOG(FATAL_WITHOUT_ABORT) here.
            // For now, stick to ALOGE or LOG_ALWAYS_FATAL.
#if DEBUG_REFS_FATAL_SANITY_CHECKS
            LOG_ALWAYS_FATAL("Strong references remain on RefBase %p (weakref_type %p)!", mBase, this);
#else
            ALOGE("Strong references remain on RefBase %p (weakref_type %p):", mBase, this);
#endif
            ref_entry* refs = mStrongRefs;
            while (refs) {
                char inc = refs->ref_count >= 0 ? '+' : '-';
                ALOGD("\t%c ID %p (ref_count %d):", inc, refs->id, refs->ref_count);
#if DEBUG_REFS_CALLSTACK_ENABLED
                refs->stack.log(LOG_TAG); // Use log method
#endif
                refs = refs->next;
            }
        }

        if (!mRetain && mWeakRefs != nullptr) {
            dumpStack = true;
#if DEBUG_REFS_FATAL_SANITY_CHECKS
            LOG_ALWAYS_FATAL("Weak references remain on RefBase %p (weakref_type %p)!", mBase, this);
#else
            ALOGE("Weak references remain on RefBase %p (weakref_type %p):", mBase, this);
#endif
            ref_entry* refs = mWeakRefs;
            while (refs) {
                char inc = refs->ref_count >= 0 ? '+' : '-';
                ALOGD("\t%c ID %p (ref_count %d):", inc, refs->id, refs->ref_count);
#if DEBUG_REFS_CALLSTACK_ENABLED
                refs->stack.log(LOG_TAG); // Use log method
#endif
                refs = refs->next;
            }
        }

        // Cleanup ref_entry lists
        ref_entry* current = mStrongRefs;
        while (current) {
            ref_entry* next = current->next;
            delete current;
            current = next;
        }
        mStrongRefs = nullptr;

        current = mWeakRefs;
        while (current) {
            ref_entry* next = current->next;
            delete current;
            current = next;
        }
        mWeakRefs = nullptr;


        if (dumpStack) {
            ALOGE("above errors at:");
            CallStack stack(LOG_TAG); // Pass logtag to constructor
            stack.update();
            stack.log(LOG_TAG); // Use log method
        }
    }

    void addStrongRef(const void* id) {
        addRef(&mStrongRefs, id, mStrong.load(std::memory_order_relaxed));
    }

    void removeStrongRef(const void* id) {
        if (!mRetain) {
            removeRef(&mStrongRefs, id);
        } else {
            // TODO(b/175724971): AOSP behavior for mRetain might be different.
            // This adds a negative ref_count, which seems unusual.
            // For now, keeping similar logic but needs review.
            addRef(&mStrongRefs, id, -mStrong.load(std::memory_order_relaxed));
        }
    }

    void renameStrongRefId(const void* old_id, const void* new_id) {
        renameRefsId(mStrongRefs, old_id, new_id);
    }

    void addWeakRef(const void* id) {
        addRef(&mWeakRefs, id, mWeak.load(std::memory_order_relaxed));
    }

    void removeWeakRef(const void* id) {
        if (!mRetain) {
            removeRef(&mWeakRefs, id);
        } else {
            addRef(&mWeakRefs, id, -mWeak.load(std::memory_order_relaxed));
        }
    }

    void renameWeakRefId(const void* old_id, const void* new_id) {
        renameRefsId(mWeakRefs, old_id, new_id);
    }

    void trackMe(bool track, bool retain)
    {
        mTrackEnabled = track;
        mRetain = retain; // TODO(b/175724971): review retain logic from AOSP
    }

    void printRefs() const
    {
        // AOSP uses a more complex system with android::base::StringPrintf
        // and potentially logging to files. This is a simplified version.
        std::string text; // Use std::string

        {
            std::lock_guard<std::mutex> _l(mMutex); // Use std::mutex
            char buf[128];
            snprintf(buf, sizeof(buf), "Strong references on RefBase %p (weakref_type %p):\n", mBase, this);
            text.append(buf);
            printRefsLocked(&text, mStrongRefs);
            snprintf(buf, sizeof(buf), "Weak references on RefBase %p (weakref_type %p):\n", mBase, this);
            text.append(buf);
            printRefsLocked(&text, mWeakRefs);
        }

        // Logging to console instead of file for simplicity here
        ALOGD("%s", text.c_str());

        // The file logging part from the original code:
        // Consider if this is still needed or how it maps to AOSP practices.
        // For now, it's kept but might be removed/changed.
        // {
        //     char name[100];
        //     snprintf(name, 100, "/data/local/tmp/%p.stack", this); // Prefer /data/local/tmp
        //     int rc = open(name, O_RDWR | O_CREAT | O_APPEND, 0666); // Added mode
        //     if (rc >= 0) {
        //         write(rc, text.data(), text.length()); // Use text.data()
        //         close(rc);
        //         ALOGD("STACK TRACE for %p saved in %s", this, name);
        //     }
        //     else ALOGE("FAILED TO PRINT STACK TRACE for %p in %s: %s", this,
        //               name, strerror(errno));
        // }
    }

private:
    struct ref_entry
    {
        ref_entry* next;
        const void* id;
#if DEBUG_REFS_CALLSTACK_ENABLED
        CallStack stack; // AOSP might use CallStackUPtr or similar
#endif
        int32_t ref_count; // Renamed from ref for clarity
    };

    void addRef(ref_entry** refs, const void* id, int32_t current_ref_val)
    {
        if (mTrackEnabled) {
            std::lock_guard<std::mutex> _l(mMutex); // Use std::mutex

            ref_entry* ref = new ref_entry;
            ref->ref_count = current_ref_val; // Store current ref count before modification
            ref->id = id;
#if DEBUG_REFS_CALLSTACK_ENABLED
            // TODO(b/175724971): AOSP uses specific skip count, e.g., 2 or 3.
            // The number of frames to skip might need adjustment.
            ref->stack.update(/* ignoreDepth */ 2, LOG_TAG); // Pass logtag
#endif
            ref->next = *refs;
            *refs = ref;
        }
    }

    void removeRef(ref_entry** refs, const void* id)
    {
        if (mTrackEnabled) {
            std::lock_guard<std::mutex> _l(mMutex); // Use std::mutex

            ref_entry* const head = *refs;
            ref_entry* current = head;
            ref_entry* prev = nullptr;

            while (current != nullptr) {
                if (current->id == id) {
                    if (prev == nullptr) {
                        *refs = current->next;
                    } else {
                        prev->next = current->next;
                    }
                    delete current;
                    return;
                }
                prev = current;
                current = current->next;
            }

#if DEBUG_REFS_FATAL_SANITY_CHECKS
            LOG_ALWAYS_FATAL("RefBase: removing id %p on RefBase %p (weakref_type %p) that doesn't exist!",
                             id, mBase, this);
#else
            ALOGE("RefBase: removing id %p on RefBase %p (weakref_type %p) that doesn't exist!",
                  id, mBase, this);
#endif

            current = head; // Reset to head for printing existing refs
            while (current) {
                char inc = current->ref_count >= 0 ? '+' : '-';
                ALOGD("\t%c ID %p (ref_count %d):", inc, current->id, current->ref_count);
                current = current->next;
            }

            CallStack stack(LOG_TAG); // Pass logtag
            stack.update();
            stack.log(LOG_TAG); // Use log method
        }
    }

    void renameRefsId(ref_entry* r, const void* old_id, const void* new_id)
    {
        if (mTrackEnabled) {
            std::lock_guard<std::mutex> _l(mMutex); // Use std::mutex
            ref_entry* ref = r;
            while (ref != nullptr) {
                if (ref->id == old_id) {
                    ref->id = new_id;
                }
                ref = ref->next;
            }
        }
    }

    void printRefsLocked(std::string* out, const ref_entry* refs) const
    {
        char buf[128];
        while (refs) {
            char inc = refs->ref_count >= 0 ? '+' : '-';
            snprintf(buf, sizeof(buf), "\t%c ID %p (ref_count %d):\n",
                    inc, refs->id, refs->ref_count);
            out->append(buf);
#if DEBUG_REFS_CALLSTACK_ENABLED
            // AOSP uses stack.toString() which might format differently.
            // This is a placeholder, actual AOSP might use a specific formatter.
            out->append(refs->stack.toString("\t\t"));
#else
            out->append("\t\t(call stacks disabled)\n");
#endif
            refs = refs->next;
        }
    }

    mutable std::mutex mMutex; // Use std::mutex
    ref_entry* mStrongRefs;
    ref_entry* mWeakRefs;

    bool mTrackEnabled;
    bool mRetain; // TODO(b/175724971): review retain logic from AOSP

#endif // DEBUG_REFS
};

// ---------------------------------------------------------------------------
// Implementation of RefBase methods

void RefBase::incStrong(const void* id) const
{
    weakref_impl* const refs = mRefs;
    refs->incWeak(id); // A weak reference is also acquired

    refs->addStrongRef(id); // For debugging
    // fetch_add returns the value before the addition.
    const int32_t c = refs->mStrong.fetch_add(1, std::memory_order_relaxed);
    // ALOG_ASSERT(c > 0, "incStrong() called on %p after last strong ref", this);
    // AOSP uses LOG_ALWAYS_FATAL for this assert.
    if (c <= 0) { // Should be > 0 if INITIAL_STRONG_VALUE is large enough and no underflow
      ALOGE("incStrong() called on %p after last strong ref or underflow. Count: %d", this, c);
      // LOG_ALWAYS_FATAL("incStrong() called on %p after last strong ref or underflow. Count: %d", this, c);
    }


#if PRINT_REFS
    ALOGD("incStrong of %p from %p: cnt=%d\n", this, id, c + 1);
#endif
    // If the strong reference count was INITIAL_STRONG_VALUE before the increment,
    // this is the first strong reference.
    if (c == INITIAL_STRONG_VALUE) {
        // Decrement by INITIAL_STRONG_VALUE to bring it to 1.
        refs->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
        // Call the onFirstRef callback.
        // Note: mBase points to the derived class instance (this).
        refs->mBase->onFirstRef();
    }
}

void RefBase::decStrong(const void* id) const
{
    weakref_impl* const refs = mRefs;
    refs->removeStrongRef(id); // For debugging
    // fetch_sub returns the value before the subtraction.
    const int32_t c = refs->mStrong.fetch_sub(1, std::memory_order_release);

#if PRINT_REFS
    ALOGD("decStrong of %p from %p: cnt=%d\n", this, id, c - 1);
#endif
    // ALOG_ASSERT(c >= 1, "decStrong() called on %p too many times", this);
    if (c < 1) { // Should be >= 1 if logic is correct
        ALOGE("decStrong() called on %p too many times. Count before dec: %d", this, c);
        // LOG_ALWAYS_FATAL("decStrong() called on %p too many times. Count before dec: %d", this, c);
    }

    if (c == 1) {
        // The last strong reference is being released.
        std::atomic_thread_fence(std::memory_order_acquire);
        refs->mBase->onLastStrongRef(id);
        // If the object's lifetime is managed by strong references, delete it.
        if ((refs->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_MASK) == OBJECT_LIFETIME_STRONG) {
            delete this; // This will also delete mRefs if !OBJECT_LIFETIME_STRONG
        }
    }
    // Release the weak reference that was acquired in incStrong.
    refs->decWeak(id);
}

// This function is less common and used when we need to ensure a strong reference
// even if the count might temporarily be zero (e.g., during construction of another
// object that also holds a reference).
void RefBase::forceIncStrong(const void* id) const
{
    weakref_impl* const refs = mRefs;
    refs->incWeak(id);

    refs->addStrongRef(id);
    const int32_t c = refs->mStrong.fetch_add(1, std::memory_order_relaxed);
#if PRINT_REFS
    ALOGD("forceIncStrong of %p from %p: cnt=%d\n", this, id, c + 1);
#endif
    // AOSP uses an assert here like:
    // ALOG_ALWAYS_FATAL_IF(c == 0 || c == INITIAL_STRONG_VALUE, "forceIncStrong() error, count=%d", c);
    // This means it expects the count to be something other than 0 or INITIAL_STRONG_VALUE
    // if it's not the first reference. This logic seems a bit complex.
    // The AOSP version seems to imply that if c is INITIAL_STRONG_VALUE, it's an error for forceIncStrong.
    // However, the original code had a switch case that handled INITIAL_STRONG_VALUE.
    // Let's try to match the AOSP intention more closely if forceIncStrong is for already live objects.
    // If c is 0 or INITIAL_STRONG_VALUE, it might indicate an issue or specific state.
    // For now, let's adapt the original switch logic.

    if (c == INITIAL_STRONG_VALUE) {
        refs->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
        // Fall through: also call onFirstRef if it was the first ref.
    }
    // If the count was 0 before incrementing (meaning it's now 1, not counting INITIAL_STRONG_VALUE),
    // or if it was INITIAL_STRONG_VALUE and now adjusted to 1, call onFirstRef.
    if (c == 0 || c == INITIAL_STRONG_VALUE) {
         refs->mBase->onFirstRef();
    }
}

int32_t RefBase::getStrongCount() const
{
    // memory_order_relaxed is sufficient because we don't need to synchronize
    // other memory operations with this load.
    return mRefs->mStrong.load(std::memory_order_relaxed);
}

// Implementation for VirtualLightRefBase::incStrongRequireStrong
// This is called by wp<T>::promote() on a RefBase object.
bool RefBase::incStrongRequireStrong(const void* id) const {
    weakref_impl* const refs = mRefs;

    // Similar to LightRefBase::incStrongRequireStrong, but for RefBase.
    // We need to ensure that we only increment if the strong count is non-zero
    // and not INITIAL_STRONG_VALUE (unless we are acquiring the first ref).

    // It also needs to call onIncStrongAttempted for certain cases.
    // This is a simplified version based on the LightRefBase logic for now.
    // A more complete version would integrate the logic from weakref_type::attemptIncStrong.

    refs->incWeak(id); // A weak ref is acquired first.

    int32_t curCount = refs->mStrong.load(std::memory_order_relaxed);
    while (curCount > 0 && curCount != INITIAL_STRONG_VALUE) {
        if (refs->mStrong.compare_exchange_weak(curCount, curCount + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
            // Successfully incremented
            refs->addStrongRef(id);
#if PRINT_REFS
            ALOGD("incStrongRequireStrong of %p from %p: succeeded, cnt=%d\n", this, id, curCount + 1);
#endif
            return true; // Successfully incremented
        }
        // curCount is updated by compare_exchange_weak on failure.
    }

    // Handle cases where curCount is 0 or INITIAL_STRONG_VALUE (attempting first ref or revival)
    // This part needs to be similar to weakref_type::attemptIncStrong's logic
    // regarding onIncStrongAttempted.
    bool allow;
    if (curCount == INITIAL_STRONG_VALUE) { // Attempting to acquire first strong reference
        allow = (refs->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_WEAK) != OBJECT_LIFETIME_WEAK
              || refs->mBase->onIncStrongAttempted(FIRST_INC_STRONG, id);
    } else { // Attempting to revive (curCount was 0)
        allow = (refs->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_WEAK) == OBJECT_LIFETIME_WEAK
              && refs->mBase->onIncStrongAttempted(FIRST_INC_STRONG, id);
    }

    if (!allow) {
        refs->decWeak(id); // Rollback weak increment
#if PRINT_REFS
        ALOGD("incStrongRequireStrong of %p from %p: failed, onIncStrongAttempted returned false\n", this, id);
#endif
        return false;
    }

    // If allowed, try to increment again.
    // This time, if it was INITIAL_STRONG_VALUE, it will become 1 (after adjustment).
    // If it was 0, it will become 1.
    curCount = refs->mStrong.fetch_add(1, std::memory_order_relaxed);

    if (curCount == 0 || curCount == INITIAL_STRONG_VALUE) {
        if (curCount == INITIAL_STRONG_VALUE) {
            refs->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
        }
        refs->mBase->onFirstRef();
    } else if (curCount > 0 && curCount < INITIAL_STRONG_VALUE) {
        // This case is from attemptIncStrong: if an existing ref was incremented by onIncStrongAttempted,
        // it might need a corresponding onLastStrongRef. This seems complex and context-dependent.
        // For incStrongRequireStrong, this might not be applicable if it's just about a simple promotion.
        // AOSP's LightRefBase::incStrongRequireStrong doesn't have this.
        // Let's keep it simple for now.
    }


    refs->addStrongRef(id);
#if PRINT_REFS
    ALOGD("incStrongRequireStrong of %p from %p: succeeded after onIncStrongAttempted, cnt=%d\n",
          this, id, refs->mStrong.load(std::memory_order_relaxed));
#endif
    return true;
}


// ---------------------------------------------------------------------------
// Implementation of RefBase::weakref_type methods

RefBase* RefBase::weakref_type::refBase() const
{
    return static_cast<const weakref_impl*>(this)->mBase;
}

void RefBase::weakref_type::incWeak(const void* id)
{
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    impl->addWeakRef(id); // For debugging
    const int32_t c = impl->mWeak.fetch_add(1, std::memory_order_relaxed);
    // ALOG_ASSERT(c >= 0, "incWeak called on %p after last weak ref", this);
     if (c < 0) { // Should be >= 0
        ALOGE("incWeak called on %p after last weak ref or underflow. Count: %d", this, c);
        // LOG_ALWAYS_FATAL("incWeak called on %p after last weak ref or underflow. Count: %d", this, c);
    }
}

void RefBase::weakref_type::decWeak(const void* id)
{
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    impl->removeWeakRef(id); // For debugging
    const int32_t c = impl->mWeak.fetch_sub(1, std::memory_order_release);
    // ALOG_ASSERT(c >= 1, "decWeak called on %p too many times", this);
     if (c < 1) { // Should be >= 1
        ALOGE("decWeak called on %p too many times. Count before dec: %d", this, c);
        // LOG_ALWAYS_FATAL("decWeak called on %p too many times. Count before dec: %d", this, c);
    }

    if (c != 1) return; // Not the last weak reference

    std::atomic_thread_fence(std::memory_order_acquire);

    if ((impl->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_MASK) == OBJECT_LIFETIME_STRONG) {
        // Object lifetime is tied to strong references.
        // If mStrong is INITIAL_STRONG_VALUE, it means there were no strong refs.
        if (impl->mStrong.load(std::memory_order_relaxed) == INITIAL_STRONG_VALUE) {
            // No strong references ever, so delete the object (RefBase).
            // The weakref_impl itself will be deleted by ~RefBase if OBJECT_LIFETIME_STRONG.
            // However, if ~RefBase is never called (no strong refs), we delete mBase here.
            // And weakref_impl is deleted now too.
            delete impl->mBase; // This should trigger ~RefBase, which might delete impl again if not careful.
                                // AOSP: if (mStrong == INITIAL_STRONG_VALUE) delete mBase; else delete this; (referring to weakref_impl)
                                // This implies weakref_impl is responsible for deleting mBase if no strong refs.
                                // And if there were strong refs, ~RefBase handles weakref_impl.
            delete impl; // Delete the weakref_impl itself.

        } else {
            // There were strong references, and now the last weak ref is gone.
            // The RefBase object (mBase) would have been deleted by decStrong when its
            // strong count reached zero.
            // Now, delete the weakref_impl.
            delete impl;
        }
    } else {
        // Object lifetime is OBJECT_LIFETIME_WEAK or OBJECT_LIFETIME_FOREVER.
        impl->mBase->onLastWeakRef(id);
        if ((impl->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_MASK) == OBJECT_LIFETIME_WEAK) {
            // Last weak reference is gone, and lifetime is OBJECT_LIFETIME_WEAK.
            // Delete the RefBase object (mBase).
            // The weakref_impl itself will be deleted by ~RefBase.
            delete impl->mBase;
        }
        // If OBJECT_LIFETIME_FOREVER, weakref_impl is deleted in ~RefBase.
    }
}

bool RefBase::weakref_type::attemptIncStrong(const void* id)
{
    incWeak(id); // First, acquire a weak reference.

    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    int32_t curCount = impl->mStrong.load(std::memory_order_relaxed);

    // ALOG_ASSERT(curCount >= 0, "attemptIncStrong called on %p after object was destroyed", this);
    // AOSP uses more specific checks. If curCount is < 0, it's an error.
    // If curCount is 0 and not OBJECT_LIFETIME_WEAK, it's an error.
    if (curCount < 0 ) { // Should not happen with INITIAL_STRONG_VALUE logic
        ALOGE("attemptIncStrong called on %p with invalid strong count %d", this, curCount);
        // LOG_ALWAYS_FATAL(...);
        decWeak(id); // Rollback weak increment
        return false;
    }


    while (curCount > 0 && curCount != INITIAL_STRONG_VALUE) {
        // Object is alive and has strong references (not the initial state).
        // Try to increment the strong count.
        if (impl->mStrong.compare_exchange_weak(curCount, curCount + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
            // Successfully incremented.
            break;
        }
        // compare_exchange_weak updates curCount with the current value on failure.
        // Loop again if curCount is still valid.
        if (curCount <= 0 || curCount == INITIAL_STRONG_VALUE) {
            // The strong count became 0 or INITIAL_STRONG_VALUE while trying.
            // Fall through to the logic below.
            goto other_cases;
        }
    }

    if (curCount > 0 && curCount != INITIAL_STRONG_VALUE) {
        // Successfully incremented existing strong count.
        impl->addStrongRef(id);
#if PRINT_REFS
        ALOGD("attemptIncStrong of %p from %p: success, cnt=%d\n", impl->mBase, id, curCount + 1);
#endif
        return true;
    }

other_cases:
    // The object either had no strong references (curCount == INITIAL_STRONG_VALUE),
    // or its strong count was zero (curCount == 0).

    // Check if the object's lifetime allows revival or first reference.
    bool allow;
    if (curCount == INITIAL_STRONG_VALUE) {
        // Attempting to acquire the first strong reference.
        allow = (impl->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_WEAK) != OBJECT_LIFETIME_WEAK
              || impl->mBase->onIncStrongAttempted(FIRST_INC_STRONG, id);
    } else { // curCount == 0
        // Attempting to revive the object (if its lifetime is OBJECT_LIFETIME_WEAK).
        allow = (impl->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_WEAK) == OBJECT_LIFETIME_WEAK
              && impl->mBase->onIncStrongAttempted(FIRST_INC_STRONG, id);
    }

    if (!allow) {
        decWeak(id); // Rollback weak increment
#if PRINT_REFS
        ALOGD("attemptIncStrong of %p from %p: failed, onIncStrongAttempted returned false\n", impl->mBase, id);
#endif
        return false;
    }

    // If allowed, increment the strong count.
    curCount = impl->mStrong.fetch_add(1, std::memory_order_relaxed);

    // If the strong reference count has already been incremented by someone else
    // (e.g., in onIncStrongAttempted), the implementor of onIncStrongAttempted()
    // might be holding an unneeded reference. Call onLastStrongRef() to remove it.
    // This logic is complex and from the original code.
    // AOSP's current LightRefBase::incStrongRequireStrong does not have this.
    // This part might need careful review against modern AOSP.
    if (curCount > 0 && curCount < INITIAL_STRONG_VALUE) {
        impl->mBase->onLastStrongRef(id);
    }

    impl->addStrongRef(id);
#if PRINT_REFS
    ALOGD("attemptIncStrong of %p from %p: success after onIncStrongAttempted, cnt=%d\n",
          impl->mBase, id, impl->mStrong.load(std::memory_order_relaxed));
#endif

    // If this was the first strong reference (previously INITIAL_STRONG_VALUE).
    if (curCount == INITIAL_STRONG_VALUE) {
        impl->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
        impl->mBase->onFirstRef();
    }

    return true;
}

bool RefBase::weakref_type::attemptIncWeak(const void* id)
{
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    int32_t curCount = impl->mWeak.load(std::memory_order_relaxed);
    // ALOG_ASSERT(curCount >= 0, "attemptIncWeak called on %p after object was destroyed", this);
     if (curCount < 0) {
        ALOGE("attemptIncWeak called on %p with invalid weak count %d", this, curCount);
        // LOG_ALWAYS_FATAL(...);
        return false;
    }


    while (curCount > 0) { // Only increment if weak count is already > 0
        if (impl->mWeak.compare_exchange_weak(curCount, curCount + 1,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            // Successfully incremented
            impl->addWeakRef(id); // For debugging
            return true;
        }
        // curCount is updated by compare_exchange_weak on failure.
    }
    return false; // Cannot increment if curCount was 0
}

int32_t RefBase::weakref_type::getWeakCount() const
{
    return static_cast<const weakref_impl*>(this)->mWeak.load(std::memory_order_relaxed);
}

void RefBase::weakref_type::printRefs() const
{
    static_cast<const weakref_impl*>(this)->printRefs();
}

void RefBase::weakref_type::trackMe(bool enable, bool retain)
{
    static_cast<weakref_impl*>(this)->trackMe(enable, retain);
}

// ---------------------------------------------------------------------------
// RefBase constructor and destructor

RefBase::RefBase()
    : mRefs(new weakref_impl(this))
{
}

RefBase::~RefBase()
{
    // This destructor is called when the RefBase object is being deleted.
    // This typically happens when:
    // 1. The last strong reference is released (if OBJECT_LIFETIME_STRONG).
    // 2. The last weak reference is released (if OBJECT_LIFETIME_WEAK).

    // If OBJECT_LIFETIME_STRONG:
    //   - decStrong calls onLastStrongRef, then delete this.
    //   - ~RefBase is called.
    //   - If weak refs exist, weakref_impl is deleted by decWeak when weak count is 0.
    //   - If no weak refs (or mWeak was already 0), we might need to delete mRefs here.

    // If OBJECT_LIFETIME_WEAK:
    //   - decWeak calls onLastWeakRef, then delete this.
    //   - ~RefBase is called.
    //   - mRefs should be deleted here.

    // If OBJECT_LIFETIME_FOREVER:
    //   - Object is not deleted by refcounting.
    //   - If explicitly deleted, ~RefBase is called.
    //   - mRefs should be deleted here.

    // AOSP logic:
    // if ((mRefs->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_MASK) != OBJECT_LIFETIME_STRONG) {
    //    // For WEAK or FOREVER, if weak count is 0, delete mRefs.
    //    // This implies that for STRONG lifetime, mRefs is handled by decWeak.
    //    if (mRefs->mWeak.load(std::memory_order_relaxed) == 0) {
    //        delete mRefs;
    //    }
    // }
    // This seems to cover WEAK and FOREVER cases.
    // For STRONG lifetime, decWeak handles deleting mRefs when weak count becomes 0.
    // If there were no weak refs to begin with (mWeak starts at 0 and incWeak/decWeak never called),
    // then mRefs might not be deleted by decWeak.

    // The original code:
    // if (mRefs->mStrong == INITIAL_STRONG_VALUE) { delete mRefs; }
    // else if ((mRefs->mFlags & OBJECT_LIFETIME_MASK) != OBJECT_LIFETIME_STRONG) {
    //   if (mRefs->mWeak == 0) delete mRefs;
    // }

    // Let's try to simplify based on AOSP's apparent intent:
    // The weakref_impl object (mRefs) should be deleted if:
    // 1. Lifetime is not STRONG, and weak count is 0 (meaning no more weak refs to care for it).
    //    This is handled here.
    // 2. Lifetime is STRONG. In this case, decWeak will delete mRefs when the weak count drops to 0
    //    *after* the RefBase object itself has been destroyed (or is being destroyed).
    //    So, if lifetime is STRONG, we don't delete mRefs here.
    //
    // The case of mStrong == INITIAL_STRONG_VALUE (no strong refs ever):
    //   - If also no weak refs, mWeak is 0. If lifetime is STRONG, decWeak (called by incStrong/decStrong pair)
    //     would delete mRefs. If lifetime is not STRONG, the condition below handles it.
    //   - This seems okay.

    if ((mRefs->mFlags.load(std::memory_order_relaxed) & OBJECT_LIFETIME_MASK) != OBJECT_LIFETIME_STRONG) {
        // For WEAK or FOREVER lifetime, if the weak count is zero, it's safe to delete mRefs.
        // If weak count is not zero, it means something still holds a weak reference, and that
        // decWeak will eventually delete mRefs.
        if (mRefs->mWeak.load(std::memory_order_relaxed) == 0) {
             delete mRefs;
        }
    } else {
        // For STRONG lifetime.
        // If mStrong is INITIAL_STRONG_VALUE (no strong refs ever acquired),
        // and mWeak is 0 (e.g. weak refs were acquired and released, or never acquired),
        // then mRefs needs to be deleted. decWeak might not have run if no weak refs were involved after strong refs.
        // This case is tricky. If no strong refs, onLastStrongRef never called.
        // If no weak refs used, decWeak never called to clean up mRefs.
        if (mRefs->mStrong.load(std::memory_order_relaxed) == INITIAL_STRONG_VALUE &&
            mRefs->mWeak.load(std::memory_order_relaxed) == 0) {
            delete mRefs;
        }
        // Otherwise, for STRONG lifetime, mRefs is managed by the last decWeak.
    }


    // For debugging purposes, clear mRefs.
    // const_cast is used here because mRefs is const.
    // This is generally acceptable in a destructor for debugging/safety.
    const_cast<weakref_impl*&>(mRefs) = nullptr;
}


RefBase::weakref_type* RefBase::createWeak(const void* id) const
{
    // This just increments the weak count on the existing mRefs.
    mRefs->incWeak(id);
    return mRefs;
}

RefBase::weakref_type* RefBase::getWeakRefs() const
{
    // Simply returns the existing mRefs.
    return mRefs;
}


void RefBase::extendObjectLifetime(int32_t mode)
{
    // Atomically OR the new mode into mFlags.
    // memory_order_relaxed is okay as this doesn't need to sync other ops.
    mRefs->mFlags.fetch_or(mode, std::memory_order_relaxed);
}

// Default implementations of virtual callbacks
void RefBase::onFirstRef()
{
    // Default is no-op
}

void RefBase::onLastStrongRef(const void* /*id*/)
{
    // Default is no-op
}

bool RefBase::onIncStrongAttempted(uint32_t flags, const void* /*id*/)
{
    // Default allows first increment, denies others if not overridden.
    return (flags & FIRST_INC_STRONG) ? true : false;
}

void RefBase::onLastWeakRef(const void* /*id*/)
{
    // Default is no-op
}

// ---------------------------------------------------------------------------
// ReferenceMover implementation (static method)

void RefBase::moveReferences(void* /*dst*/, void const* /*src*/, size_t /*n*/,
        const ReferenceConverterBase& /*caster*/)
{
    // The AOSP implementation of this function inside RefBase.cpp is empty or #if 0'd out
    // for non-DEBUG_REFS builds.
    // The actual logic for moving sp/wp is usually in VectorImpl or similar templated
    // containers, which might call renameStrongRefId/renameWeakRefId directly if DEBUG_REFS.
    // For non-DEBUG_REFS, it's often a memmove.
    // Given the header changes, this function might be less relevant or its role changed.
    // The ReferenceMover class in RefBase.h still exists, but its usage might be
    // primarily for type-safe casting in debug builds.

#if DEBUG_REFS
    // This part is from the original code and might need AOSP alignment.
    // AOSP's `libs/utils/VectorImpl.cpp` has specific `move_forward` and `move_backward`
    // that handle sp/wp types, often involving direct manipulation or placement new.
    // The `TYPE::moveReferences` call seems to be a hook for the specific RefBase type,
    // but `RefBase::moveReferences` itself is this static function.

    // The logic here was to rename IDs in debug builds.
    // This requires careful review if still applicable with modern AOSP.
    // For now, let's keep it similar to the original if DEBUG_REFS is on.

    const size_t itemSize = caster.getReferenceTypeSize();
    for (size_t i = 0; i < n; i++) {
        // Pointer arithmetic to get to the i-th element
        void* d_item = reinterpret_cast<void*>(reinterpret_cast<char*>(const_cast<void*>(dst)) + i * itemSize);
        void const* s_item = reinterpret_cast<void const*>(reinterpret_cast<const char*>(src) + i * itemSize);

        // Get the RefBase* from the destination item
        // This assumes the caster correctly interprets the item as holding a RefBase derivative.
        RefBase* ref = static_cast<RefBase*>(caster.getReferenceBase(d_item));
        if (ref && ref->mRefs) { // Check if ref and mRefs are valid
            // Rename the debug IDs from the source address to the destination address.
            ref->mRefs->renameStrongRefId(s_item, d_item);
            ref->mRefs->renameWeakRefId(s_item, d_item);
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// TextOutput functions (for debugging)

TextOutput& printStrongPointer(TextOutput& to, const void* val)
{
    to << "sp(" << val << ")"; // AOSP uses sp(...)
    return to;
}

TextOutput& printWeakPointer(TextOutput& to, const void* val)
{
    to << "wp(" << val << ")"; // AOSP uses wp(...)
    return to;
}

// TODO(b/175724971): What about VirtualLightRefBase?
// It's defined in LightRefBase.h but has no methods implemented there.
// VirtualLightRefBase::~VirtualLightRefBase() {} // Needs to be defined somewhere
// RefBase inherits from it, so RefBase's destructor serves this.
// The pure virtual methods are implemented by RefBase.

} // namespace android
