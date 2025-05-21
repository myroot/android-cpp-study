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

#ifndef ANDROID_LIGHTREFBASE_H
#define ANDROID_LIGHTREFBASE_H

#include <atomic>
#include <functional> // for std::hash
#include <type_traits> // for std::is_base_of, std::remove_cv, etc


// ---------------------------------------------------------------------------
namespace android {

template <class T>
class LightRefBase
{
public:
    inline LightRefBase() : mCount(0) { }
    inline void incStrong(__attribute__((unused)) const void* id) const {
        mCount.fetch_add(1, std::memory_order_relaxed);
    }
    inline void decStrong(__attribute__((unused)) const void* id) const {
        if (mCount.fetch_sub(1, std::memory_order_release) == 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
            delete static_cast<const T*>(this);
        }
    }
    //! DEBUGGING ONLY: Get current strong ref count.
    inline int32_t getStrongCount() const {
        return mCount.load(std::memory_order_relaxed);
    }

    // IncStrongRequireStrong returns true if the strong-count was successfully
    // incremented, false if the strong-count was 0.  This is not suitable for
    // normal use due to the TOCTOU race with decStrong().  It should only be used
    // by WeakRefBase::promote().
    //
    // If the strong-count is non-zero, it is incremented and true is returned.
    // If the strong-count is zero, it is left at zero and false is returned.
    //
    // This variant is for objects that are not also derived from WeakRefBase.
    // It is a no-op if the object is already gone.
    //
    // This function should only be used by WeakRefBase::promote().
    inline bool incStrongRequireStrong(__attribute__((unused)) const void* id) const {
      // This implementation is the same as for WeakRefBase objects for now, but
      // that may not always be the case.  For example, a separate
      // mStrongConstraint count could be added in the future.
      int32_t curCount = mCount.load(std::memory_order_relaxed);
      // we must ensure that we don't increment the strong count if it's already 0.
      // otherwise we will have a race condition with decStrong()
      // and may end up with a negative strong count.
      while (curCount != 0) {
        if (mCount.compare_exchange_weak(curCount, curCount + 1,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
          return true;
        }
        // The compare_exchange_weak failed, which means the count changed.
        // curCount has been updated with the new value. Try again.
      }
      return false;
    }

protected:
    inline ~LightRefBase() { }

private:
    friend class ReferenceMover;
    inline static void moveReferences(void* /*dst*/, void const* /*src*/, size_t /*n*/,
            const ReferenceConverterBase& /*caster*/) { }

private:
    mutable std::atomic<int32_t> mCount;
};


// This is a wrapper around LightRefBase that is virtual, for situations where
// you could not otherwise use LightRefBase.  It is used by RefBase.
class VirtualLightRefBase {
public:
    virtual ~VirtualLightRefBase();
    virtual void incStrong(const void* id) const = 0;
    virtual void decStrong(const void* id) const = 0;

    virtual int32_t getStrongCount() const = 0;

    // See comments in LightRefBase::incStrongRequireStrong().
    virtual bool incStrongRequireStrong(const void* id) const = 0;
};

}  // namespace android
// ---------------------------------------------------------------------------

namespace std {
  template<typename T> struct hash< android::LightRefBase<T> >
  {
    typedef android::LightRefBase<T> argument_type;
    typedef size_t result_type;
    result_type operator()(const argument_type& value) const
    {
      return hash<const void*>()(&value);
    }
  };
}

#endif // ANDROID_LIGHTREFBASE_H
