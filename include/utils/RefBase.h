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

#ifndef ANDROID_REF_BASE_H
#define ANDROID_REF_BASE_H

#include <atomic>
#include <functional> // for std::hash
#include <memory> // for std::unique_ptr
#include <type_traits> // for std::is_base_of, std::remove_cv, etc

#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#include <utils/LightRefBase.h>
#include <utils/StrongPointer.h>
#include <utils/TypeHelpers.h>

// ---------------------------------------------------------------------------
namespace android {

class TextOutput;
TextOutput& printWeakPointer(TextOutput& to, const void* val);

// Macro for comparing weak pointers
// TODO(b/175724971): this should be removed.
#define COMPARE_WEAK(_op_)                                                                        \
    template <typename U>                                                                         \
    inline bool operator _op_(const sp<U>& o) const {                                             \
        return m_ptr _op_ o.m_ptr;                                                                \
    }                                                                                             \
    template <typename U>                                                                         \
    inline bool operator _op_(const U* o) const {                                                 \
        return m_ptr _op_ o;                                                                      \
    }                                                                                             \
    /* This comparison is an error: comparing a weak pointer with a strong pointer is not safe. */ \
    /* Use promote().get() if you are sure that the strong pointer is not null. */                \
    /* inline bool operator _op_(const sp<T>& o) const; */                                        \
    /* To see the error, remove the space before inline: */                                       \
     inline bool operator _op_(const T* o) const {                                                \
        return m_ptr _op_ o;                                                                      \
    }

// ---------------------------------------------------------------------------
// Forward declarations
class ReferenceMover;

// Interface for converting pointers to RefBase::basetype*.
// This is used by RefBase::moveReferences to safely cast pointers.
class ReferenceConverterBase {
  public:
    virtual ~ReferenceConverterBase() = default;
    virtual size_t getReferenceTypeSize() const = 0;
    virtual RefBase::basetype* getReferenceBase(void const*) const = 0;
};

// ---------------------------------------------------------------------------

// RefBase is the base class for smart pointer objects.
//
// The design of RefBase is heavily influenced by the Windows COM programming
// model, specifically the IUnknown interface and its AddRef and Release methods.
//
// Key features:
//
// - Strong pointers (sp): These control the object's lifetime. When the last
//   strong pointer is released, the object is destroyed.
// - Weak pointers (wp): These allow observing an object without extending its
//   lifetime. They can be "promoted" to strong pointers if the object is still
//   alive.
// - Thread safety: Reference counting is thread-safe using std::atomic.
// - Lifecycle hooks: Virtual methods like onFirstRef, onLastStrongRef, etc.,
//   allow derived classes to customize behavior at different lifecycle stages.
// - Debugging features: Extensive debugging support is available when
//   DEBUG_REFS is enabled, including reference tracking and leak detection.
//
// Usage guidelines:
//
// - Objects inheriting from RefBase should generally be created using
//   `sp::make` or `new` and immediately assigned to an `sp`.
// - Avoid storing raw pointers to RefBase objects. Use `sp` or `wp` instead.
// - Be careful with cyclic references. If object A holds an `sp` to object B,
//   and object B holds an `sp` to object A, neither object will be destroyed.
//   Use `wp` to break such cycles.
// - When passing RefBase objects as function arguments, use `const sp<T>&` or
//   `const wp<T>&` for input parameters, and `sp<T>` for output parameters or
//   return values.
//
class RefBase : public VirtualLightRefBase {
public:
    void incStrong(const void* id) const final;
    void decStrong(const void* id) const final;

    void forceIncStrong(const void* id) const;

    int32_t getStrongCount() const override;

    // See comments in LightRefBase::incStrongRequireStrong().
    // This variant is for RefBase objects.
    bool incStrongRequireStrong(const void* id) const final;

    class weakref_type
    {
    public:
        RefBase* refBase() const;

        void incWeak(const void* id);
        void decWeak(const void* id);

        // acquires a strong reference if there is already one.
        bool attemptIncStrong(const void* id);

        // acquires a weak reference if there is already one.
        // This is not always safe. see ProcessState.cpp and BpBinder.cpp
        // for proper use.
        bool attemptIncWeak(const void* id);

        int32_t getWeakCount() const;

        void printRefs() const;
        void trackMe(bool enable, bool retain);
    };

    weakref_type* createWeak(const void* id) const;
    weakref_type* getWeakRefs() const;

    void printRefs() const { getWeakRefs()->printRefs(); }
    void trackMe(bool enable, bool retain) { getWeakRefs()->trackMe(enable, retain); }

    // This is a part of the VirtualLightRefBase interface.
    // RefBase objects are never LightRefBase, so this is a no-op.
    // TODO(b/175724971): should this return a status or assert?
    // Maybe it should not be part of VirtualLightRefBase.
    // This function should only be used by WeakRefBase::promote().
    // bool incStrongRequireStrong(const void* id) const override { return true; }

    // The basetype of RefBase is VirtualLightRefBase.
    // TODO(b/175724971): This is confusing. It means that
    // static_cast<basetype*>(refbase_ptr) will return a VirtualLightRefBase*.
    // But sp<RefBase> will store a RefBase*.
    // Should this be removed or changed?
    using basetype = VirtualLightRefBase;

protected:
    RefBase();
    ~RefBase() override;

    enum {
        OBJECT_LIFETIME_STRONG = 0x0000,
        OBJECT_LIFETIME_WEAK = 0x0001, // an object with only weak refs can be destroyed
        OBJECT_LIFETIME_MASK = 0x0001
    };

    void extendObjectLifetime(int32_t mode);

    enum {
        FIRST_INC_STRONG = 0x0001
    };

    // Invoked when the first strong reference is created.
    // Implement this method to perform actions that should happen once the object
    // is about to become "live".
    virtual void onFirstRef();

    // Invoked when the last strong reference is released.
    // Implement this method to perform actions that should happen once the object
    // is no longer strongly referenced. The object will be destroyed soon after
    // this method returns, unless a new strong reference is acquired.
    virtual void onLastStrongRef(const void* id);

    // Invoked when a strong reference is attempted on an object that already has
    // strong references.
    // The 'flags' argument can be used to pass information about the context of
    // the increment. Currently, only FIRST_INC_STRONG is defined.
    //
    // Return true if the increment is allowed, false otherwise.
    // The default implementation returns true.
    virtual bool onIncStrongAttempted(uint32_t flags, const void* id);

    // Invoked when the last weak reference is released.
    // Implement this method to perform actions that should happen when the object
    // is no longer referenced by any weak pointers.
    virtual void onLastWeakRef(const void* id);

private:
    friend class ReferenceMover;
    // moveReferences is used by VectorImpl to move sp<> and wp<> objects.
    // It is not part of the public API and should not be called directly.
    static void moveReferences(void* d, void const* s, size_t n, const ReferenceConverterBase& caster);

private:
    friend class weakref_type;
    class weakref_impl;

    RefBase(const RefBase& o); // = delete
    RefBase& operator=(const RefBase& o); // = delete

    weakref_impl* const mRefs;
};

// ---------------------------------------------------------------------------

// A weak pointer (wp) holds a non-owning reference to an object.
//
// It is used to observe an object without extending its lifetime.
// A wp can be "promoted" to a strong pointer (sp) if the object is still alive.
// This is useful for breaking reference cycles.
//
// The wp class is template-based and can be used with any type that inherits
// from RefBase.
//
// Key features:
//
// - Non-owning: Does not affect the object's lifetime.
// - Promotion: Can be converted to an sp using the promote() method.
// - Thread safety: Operations on wp objects are thread-safe.
//
// Usage guidelines:
//
// - Use wp to break reference cycles. If object A holds an sp to object B, and
//   object B needs to refer back to object A, B should hold a wp to A.
// - Always check the result of promote() before using the pointer. If promote()
//   returns a null sp, the object has been destroyed.
// - wp objects are lightweight and can be copied freely.
//
template <typename T>
class wp
{
public:
    using weakref_type = typename RefBase::weakref_type;

    inline wp() : m_ptr(nullptr), m_refs(nullptr) { }

    // Constructs a weak pointer from a raw pointer to T.
    // The raw pointer must be to an object that inherits from RefBase.
    //
    // Example:
    //   MyClass* rawPtr = new MyClass();
    //   wp<MyClass> weakPtr(rawPtr);
    //
    // Note: It is generally recommended to use sp::make or new and immediately
    // assign to an sp to create RefBase objects.
    wp(T* other);

    // Constructs a weak pointer from another weak pointer.
    wp(const wp<T>& other);

    // Constructs a weak pointer from a strong pointer.
    wp(const sp<T>& other);

    // Constructs a weak pointer from a raw pointer to U, where U is a type
    // that is convertible to T.
    template<typename U> wp(U* other);

    // Constructs a weak pointer from a strong pointer to U, where U is a type
    // that is convertible to T.
    template<typename U> wp(const sp<U>& other);

    // Constructs a weak pointer from another weak pointer to U, where U is a type
    // that is convertible to T.
    template<typename U> wp(const wp<U>& other);

    ~wp();

    // Assignment operators
    wp& operator = (T* other);
    wp& operator = (const wp<T>& other);
    wp& operator = (const sp<T>& other);

    template<typename U> wp& operator = (U* other);
    template<typename U> wp& operator = (const wp<U>& other);
    template<typename U> wp& operator = (const sp<U>& other);

    // Sets the pointer and refs directly. Internal use only.
    void set_object_and_refs(T* other, weakref_type* refs);

    // Promotes the weak pointer to a strong pointer.
    // If the object is still alive, returns an sp to the object.
    // Otherwise, returns a null sp.
    sp<T> promote() const;

    // Resets the weak pointer to null.
    void clear();

    // Accessors
    inline weakref_type* get_refs() const { return m_refs; }
    inline T* unsafe_get() const { return m_ptr; } // Use with caution!

    // Operators
    // TODO(b/175724971): these should be removed.
    COMPARE_WEAK(==)
    COMPARE_WEAK(!=)
    // COMPARE_WEAK(>)  // Not supported by AOSP
    // COMPARE_WEAK(<)  // Not supported by AOSP
    // COMPARE_WEAK(<=) // Not supported by AOSP
    // COMPARE_WEAK(>=) // Not supported by AOSP

    inline bool operator == (const wp<T>& o) const {
        return (m_ptr == o.m_ptr) && (m_refs == o.m_refs);
    }
    template<typename U>
    inline bool operator == (const wp<U>& o) const {
        // Implicit promotion is costly and can lead to subtle bugs.
        // static_assert(!std::is_base_of<RefBase, U>::value, "Implicit promotion in operator== is not allowed.");
        return m_ptr == o.m_ptr; // This may not be safe if U is not related to T.
    }

    // Explicit bool conversion. Returns true if the pointer is not null.
    explicit operator bool() const { return m_ptr != nullptr; }


    // TODO(b/175724971): The following operators are not in AOSP
    // and should be removed or carefully reviewed.
    /*
    inline bool operator > (const wp<T>& o) const {
        return (m_ptr == o.m_ptr) ? (m_refs > o.m_refs) : (m_ptr > o.m_ptr);
    }
    template<typename U>
    inline bool operator > (const wp<U>& o) const {
        return (m_ptr == o.m_ptr) ? (m_refs > o.m_refs) : (m_ptr > o.m_ptr);
    }

    inline bool operator < (const wp<T>& o) const {
        return (m_ptr == o.m_ptr) ? (m_refs < o.m_refs) : (m_ptr < o.m_ptr);
    }
    template<typename U>
    inline bool operator < (const wp<U>& o) const {
        return (m_ptr == o.m_ptr) ? (m_refs < o.m_refs) : (m_ptr < o.m_ptr);
    }

    inline bool operator != (const wp<T>& o) const { return m_refs != o.m_refs; } // This seems incorrect.
    template<typename U> inline bool operator != (const wp<U>& o) const { return !operator == (o); }
    inline bool operator <= (const wp<T>& o) const { return !operator > (o); }
    template<typename U> inline bool operator <= (const wp<U>& o) const { return !operator > (o); }
    inline bool operator >= (const wp<T>& o) const { return !operator < (o); }
    template<typename U> inline bool operator >= (const wp<U>& o) const { return !operator < (o); }
    */

    // Creates a wp from an existing RefBase object.
    // This is useful when you have a raw pointer to a RefBase object that you
    // know is already managed by an sp, and you want to create a wp to it.
    //
    // Example:
    //   sp<MyClass> strongPtr = sp<MyClass>::make();
    //   MyClass* rawPtr = strongPtr.get();
    //   wp<MyClass> weakPtr = wp<MyClass>::fromExisting(rawPtr);
    //
    // Note: This method should be used with caution. If the raw pointer is not
    // actually managed by an sp, this can lead to undefined behavior.
    static wp<T> fromExisting(T* ptr);

private:
    template<typename Y> friend class sp;
    template<typename Y> friend class wp;

    T* m_ptr;
    weakref_type* m_refs;
};

template <typename T>
TextOutput& operator<<(TextOutput& to, const wp<T>& val);

#undef COMPARE_WEAK

// ---------------------------------------------------------------------------
// No user serviceable parts below here.

template<typename T>
wp<T>::wp(T* other)
    : m_ptr(other)
{
    if (other) m_refs = other->createWeak(this);
}

template<typename T>
wp<T>::wp(const wp<T>& other)
    : m_ptr(other.m_ptr), m_refs(other.m_refs)
{
    if (m_ptr) m_refs->incWeak(this);
}

template<typename T>
wp<T>::wp(const sp<T>& other)
    : m_ptr(other.get()) // Use .get() to access the raw pointer in sp
{
    if (m_ptr) {
        m_refs = m_ptr->createWeak(this);
    } else {
        m_refs = nullptr; // Ensure m_refs is null if m_ptr is null
    }
}

template<typename T> template<typename U>
wp<T>::wp(U* other)
    : m_ptr(static_cast<T*>(other)) // Perform static_cast for cross-type construction
{
    if (other) m_refs = other->createWeak(this);
}

template<typename T> template<typename U>
wp<T>::wp(const wp<U>& other)
    : m_ptr(static_cast<T*>(other.m_ptr)), m_refs(other.m_refs)
{
    if (m_ptr) {
        m_refs->incWeak(this);
    }
}

template<typename T> template<typename U>
wp<T>::wp(const sp<U>& other)
    : m_ptr(static_cast<T*>(other.get())) // Use .get() and static_cast
{
    if (m_ptr) {
        m_refs = m_ptr->createWeak(this);
    } else {
        m_refs = nullptr;
    }
}

template<typename T>
wp<T>::~wp()
{
    if (m_ptr) m_refs->decWeak(this);
}

template<typename T>
wp<T>& wp<T>::operator = (T* other)
{
    weakref_type* newRefs = other ? other->createWeak(this) : nullptr;
    if (m_ptr) m_refs->decWeak(this);
    m_ptr = other;
    m_refs = newRefs;
    return *this;
}

template<typename T>
wp<T>& wp<T>::operator = (const wp<T>& other)
{
    if (this != &other) { // Protect against self-assignment
        weakref_type* otherRefs = other.m_refs;
        T* otherPtr = other.m_ptr;
        if (otherPtr) otherRefs->incWeak(this);
        if (m_ptr) m_refs->decWeak(this);
        m_ptr = otherPtr;
        m_refs = otherRefs;
    }
    return *this;
}

template<typename T>
wp<T>& wp<T>::operator = (const sp<T>& other)
{
    weakref_type* newRefs = other ? other->createWeak(this) : nullptr;
    T* otherPtr = other.get();
    if (m_ptr) m_refs->decWeak(this);
    m_ptr = otherPtr;
    m_refs = newRefs;
    return *this;
}

template<typename T> template<typename U>
wp<T>& wp<T>::operator = (U* other)
{
    weakref_type* newRefs = other ? other->createWeak(this) : nullptr;
    if (m_ptr) m_refs->decWeak(this);
    m_ptr = static_cast<T*>(other);
    m_refs = newRefs;
    return *this;
}

template<typename T> template<typename U>
wp<T>& wp<T>::operator = (const wp<U>& other)
{
    weakref_type* otherRefs = other.m_refs;
    U* otherRawPtr = other.m_ptr; // Use U* here
    T* otherPtr = static_cast<T*>(otherRawPtr); // Cast to T*
    if (otherPtr) otherRefs->incWeak(this);
    if (m_ptr) m_refs->decWeak(this);
    m_ptr = otherPtr;
    m_refs = otherRefs;
    return *this;
}

template<typename T> template<typename U>
wp<T>& wp<T>::operator = (const sp<U>& other)
{
    weakref_type* newRefs = other ? other->createWeak(this) : nullptr;
    U* otherRawPtr = other.get(); // Use U* here
    T* otherPtr = static_cast<T*>(otherRawPtr); // Cast to T*
    if (m_ptr) m_refs->decWeak(this);
    m_ptr = otherPtr;
    m_refs = newRefs;
    return *this;
}

template<typename T>
void wp<T>::set_object_and_refs(T* other, weakref_type* refs)
{
    if (other) refs->incWeak(this); // This should inc ref on new object
    if (m_ptr) m_refs->decWeak(this); // Dec ref on old object
    m_ptr = other;
    m_refs = refs;
}

template<typename T>
sp<T> wp<T>::promote() const
{
    // Ternary operator is more concise here
    return (m_ptr && m_refs->attemptIncStrong(&std::remove_pointer<sp<T>>::type::sDum))
           ? sp<T>::fromExisting(m_ptr) // Use sp::fromExisting to create sp from raw ptr
           : sp<T>(nullptr);
}

template<typename T>
void wp<T>::clear()
{
    if (m_ptr) {
        m_refs->decWeak(this);
        m_ptr = nullptr;
        m_refs = nullptr; // Also clear m_refs
    }
}

template<typename T>
/*static*/ wp<T> wp<T>::fromExisting(T* ptr) {
    wp<T> res;
    if (ptr) {
        res.m_ptr = ptr;
        res.m_refs = ptr->getWeakRefs(); // getWeakRefs instead of createWeak
        if (res.m_refs) {
             // No incWeak here, as we are observing an existing ref-counted object
        } else {
            // This case should ideally not happen if ptr is a valid RefBase object
            // that has been properly initialized (e.g. by an sp).
            // If m_refs is null, it means createWeak was never called on ptr,
            // or it's already being destroyed.
            // Setting m_ptr to null to indicate an invalid wp.
            res.m_ptr = nullptr;
        }
    }
    return res;
}


template <typename T>
inline TextOutput& operator<<(TextOutput& to, const wp<T>& val)
{
    // TODO(b/175724971): This should promote and print the sp, or print raw ptr if null.
    return printWeakPointer(to, val.unsafe_get());
}

// ---------------------------------------------------------------------------
// For static analysis/tools only. Not for use in production code.
// TODO(b/175724971): This section needs to be reviewed and updated.
// It seems to be related to the old type system and may not be relevant anymore.
class ReferenceMover {
    template <typename TYPE>
    struct StrongReferenceCast : public ReferenceConverterBase {
        size_t getReferenceTypeSize() const override { return sizeof(sp<TYPE>); }
        RefBase::basetype* getReferenceBase(void const* p) const override {
            sp<TYPE> const* sptr(reinterpret_cast<sp<TYPE> const*>(p));
            // TODO(b/175724971): This cast is problematic.
            // TYPE::basetype might not be VirtualLightRefBase.
            // It should probably cast to RefBase* and then to VirtualLightRefBase*.
            return static_cast<typename TYPE::basetype*>(sptr->get());
        }
    };

    template <typename TYPE>
    struct WeakReferenceCast : public ReferenceConverterBase {
        size_t getReferenceTypeSize() const override { return sizeof(wp<TYPE>); }
        RefBase::basetype* getReferenceBase(void const* p) const override {
            wp<TYPE> const* wptr(reinterpret_cast<wp<TYPE> const*>(p));
            // TODO(b/175724971): Similar to StrongReferenceCast, this cast is problematic.
            return static_cast<typename TYPE::basetype*>(wptr->unsafe_get());
        }
    };

public:
    template<typename TYPE> static inline
    void move_references(sp<TYPE>* d, sp<TYPE> const* s, size_t n) {
        // This is a bitwise move. It does not call constructors or destructors.
        // This is only safe if sp<TYPE> is a POD type, which it is not.
        // TODO(b/175724971): This function is unsafe and should be removed or redesigned.
        // It is currently used by VectorImpl.
        memmove(d, s, n*sizeof(sp<TYPE>));
#if 0 // This part is likely incorrect and disabled in AOSP.
        StrongReferenceCast<TYPE> caster;
        TYPE::moveReferences(d, s, n, caster);
#endif
    }

    template<typename TYPE> static inline
    void move_references(wp<TYPE>* d, wp<TYPE> const* s, size_t n) {
        // Similar to the sp version, this is a bitwise move and is unsafe.
        // TODO(b/175724971): This function is unsafe and should be removed or redesigned.
        memmove(d, s, n*sizeof(wp<TYPE>));
#if 0 // This part is likely incorrect and disabled in AOSP.
        WeakReferenceCast<TYPE> caster;
        TYPE::moveReferences(d, s, n, caster);
#endif
    }
};

// Specializations for moving sp<> and wp<> types.
// These are used by VectorImpl and friends.
// TODO(b/175724971): These functions are unsafe and should be removed or redesigned.
template<typename TYPE> inline
void move_forward_type(sp<TYPE>* d, sp<TYPE> const* s, size_t n) {
    ReferenceMover::move_references(d, s, n);
}

template<typename TYPE> inline
void move_backward_type(sp<TYPE>* d, sp<TYPE> const* s, size_t n) {
    ReferenceMover::move_references(d, s, n);
}

template<typename TYPE> inline
void move_forward_type(wp<TYPE>* d, wp<TYPE> const* s, size_t n) {
    ReferenceMover::move_references(d, s, n);
}

template<typename TYPE> inline
void move_backward_type(wp<TYPE>* d, wp<TYPE> const* s, size_t n) {
    ReferenceMover::move_references(d, s, n);
}

}; // namespace android

// Provide std::hash specializations for sp and wp.
namespace std {
    template<typename T> struct hash< android::sp<T> >
    {
        typedef android::sp<T> argument_type;
        typedef size_t result_type;
        result_type operator()(const argument_type& value) const
        {
            return hash<const void*>()(value.get());
        }
    };

    template<typename T> struct hash< android::wp<T> >
    {
        typedef android::wp<T> argument_type;
        typedef size_t result_type;
        result_type operator()(const argument_type& value) const
        {
            // Hash the raw pointer. Note that this means that a wp to a dead
            // object will have the same hash as a wp to a live object if they
            // originally pointed to the same address.
            return hash<const void*>()(value.unsafe_get());
        }
    };
}  // namespace std
// ---------------------------------------------------------------------------
#endif // ANDROID_REF_BASE_H
