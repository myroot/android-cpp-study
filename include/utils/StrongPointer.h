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

#ifndef ANDROID_STRONG_POINTER_H
#define ANDROID_STRONG_POINTER_H

#include <functional> // For std::hash
#include <type_traits> // For std::is_convertible, std::enable_if, etc.
#include <utility> // For std::move, std::forward

#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
namespace android {

class TextOutput;
TextOutput& printStrongPointer(TextOutput& to, const void* val);

template<typename T> class wp;
template<typename T> class sp;

// ---------------------------------------------------------------------------
// Standard comparison operators for sp and wp.
// TODO(b/175724971): These should be non-member functions and potentially removed
// if they cause issues with C++20's defaulted comparison operators.
#define COMPARE_STRONG(_op_)                                                                 \
    inline bool operator _op_(const sp<T>& o) const { return m_ptr _op_ o.m_ptr; }            \
    inline bool operator _op_(const T* o) const { return m_ptr _op_ o; }                      \
    /* Removed comparison with wp here, as it's generally unsafe */                          \
    /* and requires promotion. */                                                            \
    template <typename U>                                                                    \
    inline bool operator _op_(const sp<U>& o) const { return m_ptr _op_ o.m_ptr; }            \
    template <typename U>                                                                    \
    inline bool operator _op_(const U* o) const { return m_ptr _op_ o; }

// The ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION macro can be used to
// disable implicit construction of sp<T> from T* or sp<U> from U* where U is
// a type that is convertible to T. This can help prevent accidental ownership
// transfer or unintended reference count increments.
//
// If defined, constructors from raw pointers or convertible sp types must be
// explicit or use sp::fromExisting or sp::make.
//
// #define ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION 1

// ---------------------------------------------------------------------------

// The sp class is a smart pointer that manages the lifetime of a RefBase object.
//
// It provides strong ownership of the object, meaning that the object will be
// destroyed when the last sp pointing to it is released.
//
// Key features:
//
// - Automatic memory management: Objects are automatically deleted when no
//   longer referenced.
// - Thread safety: Reference counting is thread-safe.
// - Type safety: sp objects are strongly typed.
// - Interoperability with wp: sp objects can be converted to wp (weak pointer)
//   objects, which do not extend the lifetime of the object.
//
// Usage guidelines:
//
// - Always initialize sp objects. A default-constructed sp is null.
// - Use sp::make to create and initialize sp objects with new objects.
//   Example: sp<MyClass> ptr = sp<MyClass>::make();
// - Use sp::fromExisting when you have a raw pointer to an object that is
//   already managed by another sp, and you want to share ownership.
//   Example: sp<MyClass> newPtr = sp<MyClass>::fromExisting(rawPtr);
// - Avoid storing raw pointers to RefBase objects. Use sp or wp instead.
// - Be careful with cyclic references. If object A holds an sp to object B,
//   and object B holds an sp to object A, neither object will be destroyed.
//   Use wp to break such cycles.
//
template <typename T>
class sp
{
public:
    // For interfacing with RefBase::weakref_type::attemptIncStrong()
    inline static void sDum() {} // A no-op static member function

    // Default constructor initializes to nullptr.
    inline sp() : m_ptr(nullptr) { }

    // Constructors from raw pointer T*.
#ifdef ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION
    explicit sp(T* other);
#else
    sp(T* other);
#endif

    // Copy constructor.
    sp(const sp<T>& other);

    // Constructor from nullptr.
    sp(std::nullptr_t); // NOLINT(google-explicit-constructor)

    // Constructors from convertible types U* and sp<U>.
    // These are enabled only if U* is convertible to T*.
    template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
#ifdef ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION
    explicit sp(U* other);
#else
    sp(U* other); // NOLINT(google-explicit-constructor)
#endif

    template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
#ifdef ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION
    explicit sp(const sp<U>& other);
#else
    sp(const sp<U>& other); // NOLINT(google-explicit-constructor)
#endif

    // Move constructor.
    sp(sp<T>&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }

    // Move constructor from sp<U>
    template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
    sp(sp<U>&& other) : m_ptr(other.m_ptr) {
        // static_cast<sp<U>&>(other).m_ptr = nullptr; // This is one way, or use release()
        other.m_ptr = nullptr; // Simpler, relying on sp<U>'s structure
    }


    ~sp();

    // Assignment operators

    // Assign from raw pointer T*.
    sp& operator = (T* other);

    // Copy assignment.
    sp& operator = (const sp<T>& other);

    // Assign from nullptr.
    sp& operator = (std::nullptr_t);

    // Assign from convertible sp<U>.
    template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
    sp& operator = (const sp<U>& other);

    // Assign from convertible U*.
    template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
    sp& operator = (U* other);

    // Move assignment.
    sp& operator = (sp<T>&& other) noexcept {
        T* oldPtr = m_ptr;
        m_ptr = other.m_ptr;
        other.m_ptr = nullptr;
        if (oldPtr) oldPtr->decStrong(this);
        return *this;
    }

    // Move assignment from sp<U>
    template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
    sp& operator = (sp<U>&& other) {
        T* oldPtr = m_ptr;
        m_ptr = other.m_ptr; // other.m_ptr is of type U* but convertible to T*
        // static_cast<sp<U>&>(other).m_ptr = nullptr; // Or other.release() if it returns U*
        other.m_ptr = nullptr;
        if (oldPtr) oldPtr->decStrong(this);
        return *this;
    }


    //! Special optimization for use by ProcessState (and nobody else).
    void force_set(T* other);

    // Resets the sp to null, releasing the reference to the object.
    void clear();

    // Releases ownership of the raw pointer without decrementing the ref count.
    // The caller is responsible for managing the lifetime of the returned pointer.
    // This is potentially dangerous and should be used with caution.
    T* release();

    // Accessors
    inline T& operator* () const  { return *m_ptr; }
    inline T* operator-> () const { return m_ptr;  } // NOLINT(google-runtime-operator)
    inline T* get() const         { return m_ptr; }

    // Explicit bool conversion. Returns true if the pointer is not null.
    explicit operator bool() const { return m_ptr != nullptr; }


    // Comparison operators
    // TODO(b/175724971): Review these against C++20 defaulted comparisons.
    // For now, keeping the macro-based approach for compatibility.
    COMPARE_STRONG(==)
    COMPARE_STRONG(!=)
    COMPARE_STRONG(>)
    COMPARE_STRONG(<)
    COMPARE_STRONG(<=)
    COMPARE_STRONG(>=)


    // Factory methods

    // Creates a new object of type U (which must be T or derived from T)
    // and returns an sp to it.
    //
    // Example:
    //   sp<MyClass> ptr = sp<MyClass>::make(); // Default constructs MyClass
    //   sp<MyClass> ptr2 = sp<MyClass>::make(arg1, arg2); // Constructs MyClass with args
    template<typename... Args>
    static sp<T> make(Args&&... args);

    // Creates an sp from an existing raw pointer that is already ref-counted.
    // This is used when you have a T* that you know is managed by another sp,
    // and you want to share ownership by creating a new sp.
    // This function simply increments the strong count of the existing pointer.
    //
    // Caution: If ptr is not actually managed by a ref-counting mechanism,
    // this will lead to undefined behavior.
    static sp<T> fromExisting(T* ptr);


    // Casts an sp<U> to sp<T>.
    // This is equivalent to static_cast<T*>(uPtr.get()) but returns an sp.
    // Requires U* to be static_cast-able to T*.
    template <typename U>
    static sp<T> cast(const sp<U>& uPtr);

    // TODO(b/175724971): Add dynamic_cast equivalent if needed.

private:
    template<typename Y> friend class sp;
    template<typename Y> friend class wp;

    // For construction from wp::promote()
    // This constructor is private and should only be called by wp::promote().
    // It assumes that ptr is valid and its strong reference count has already
    // been incremented.
    // TODO(b/175724971): This was used in some older versions. Current AOSP uses fromExisting.
    // Keeping it commented for now.
    // explicit sp(T* p, bool inc);


    // Internal method to set the pointer, used by constructors and assignment ops.
    // Handles incStrong/decStrong logic.
    void set_pointer(T* ptr);


    T* m_ptr;
};

#undef COMPARE_STRONG // Undefine the macro after use

// Non-member comparison operators for sp<T> with nullptr
// TODO(b/175724971): Ensure these are consistent with C++20 practices.
template<typename T>
inline bool operator==(const sp<T>& p, std::nullptr_t) { return p.get() == nullptr; }
template<typename T>
inline bool operator==(std::nullptr_t, const sp<T>& p) { return p.get() == nullptr; }
template<typename T>
inline bool operator!=(const sp<T>& p, std::nullptr_t) { return p.get() != nullptr; }
template<typename T>
inline bool operator!=(std::nullptr_t, const sp<T>& p) { return p.get() != nullptr; }
template<typename T>
inline bool operator<(const sp<T>& p, std::nullptr_t) { return std::less<T*>()(p.get(), nullptr); }
template<typename T>
inline bool operator<(std::nullptr_t, const sp<T>& p) { return std::less<T*>()(nullptr, p.get()); }
template<typename T>
inline bool operator<=(const sp<T>& p, std::nullptr_t) { return !(nullptr < p); }
template<typename T>
inline bool operator<=(std::nullptr_t, const sp<T>& p) { return !(p < nullptr); }
template<typename T>
inline bool operator>(const sp<T>& p, std::nullptr_t) { return nullptr < p; }
template<typename T>
inline bool operator>(std::nullptr_t, const sp<T>& p) { return p < nullptr; }
template<typename T>
inline bool operator>=(const sp<T>& p, std::nullptr_t) { return !(p < nullptr); }
template<typename T>
inline bool operator>=(std::nullptr_t, const sp<T>& p) { return !(nullptr < p); }


template <typename T>
TextOutput& operator<<(TextOutput& to, const sp<T>& val);

// ---------------------------------------------------------------------------
// No user serviceable parts below here.
// (Implementation of sp methods)

template<typename T>
sp<T>::sp(T* other) : m_ptr(other) {
    if (other) other->incStrong(this);
}

template<typename T>
sp<T>::sp(const sp<T>& other) : m_ptr(other.m_ptr) {
    if (m_ptr) m_ptr->incStrong(this);
}

template<typename T>
sp<T>::sp(std::nullptr_t) : m_ptr(nullptr) {}


template<typename T> template<typename U, typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
sp<T>::sp(U* other) : m_ptr(static_cast<T*>(other)) { // static_cast for cross-type construction
    if (m_ptr) m_ptr->incStrong(this);
}

template<typename T> template<typename U, typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
sp<T>::sp(const sp<U>& other) : m_ptr(static_cast<T*>(other.m_ptr)) { // static_cast for cross-type copy
    if (m_ptr) m_ptr->incStrong(this);
}

template<typename T>
sp<T>::~sp() {
    if (m_ptr) m_ptr->decStrong(this);
}

template<typename T>
sp<T>& sp<T>::operator=(const sp<T>& other) {
    if (this != &other) { // Protect against self-assignment
        T* oldPtr = m_ptr;
        m_ptr = other.m_ptr;
        if (m_ptr) m_ptr->incStrong(this);
        if (oldPtr) oldPtr->decStrong(this);
    }
    return *this;
}

template<typename T>
sp<T>& sp<T>::operator=(T* other) {
    T* oldPtr = m_ptr;
    m_ptr = other;
    if (m_ptr) m_ptr->incStrong(this);
    if (oldPtr) oldPtr->decStrong(this);
    return *this;
}

template<typename T>
sp<T>& sp<T>::operator=(std::nullptr_t) {
    if (m_ptr) {
        m_ptr->decStrong(this);
        m_ptr = nullptr;
    }
    return *this;
}


template<typename T> template<typename U, typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
sp<T>& sp<T>::operator=(const sp<U>& other) {
    T* oldPtr = m_ptr;
    m_ptr = static_cast<T*>(other.m_ptr); // static_cast for cross-type assignment
    if (m_ptr) m_ptr->incStrong(this);
    if (oldPtr) oldPtr->decStrong(this);
    return *this;
}

template<typename T> template<typename U, typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
sp<T>& sp<T>::operator=(U* other) {
    T* oldPtr = m_ptr;
    m_ptr = static_cast<T*>(other); // static_cast for cross-type assignment
    if (m_ptr) m_ptr->incStrong(this);
    if (oldPtr) oldPtr->decStrong(this);
    return *this;
}

template<typename T>
void sp<T>::force_set(T* other) {
    // This function assumes 'other' is a valid pointer to a RefBase object.
    // It forcefully increments the strong count, bypassing regular incStrong logic.
    // Used in specific scenarios like ProcessState where the object's lifetime
    // is managed differently.
    if (other) other->forceIncStrong(this); // Ensure 'other' is not null
    if (m_ptr) m_ptr->decStrong(this); // Decrement old pointer if exists
    m_ptr = other;
}

template<typename T>
void sp<T>::clear() {
    if (m_ptr) {
        m_ptr->decStrong(this);
        m_ptr = nullptr;
    }
}

template<typename T>
T* sp<T>::release() {
    T* releasedPtr = m_ptr;
    m_ptr = nullptr;
    return releasedPtr;
}


template<typename T>
void sp<T>::set_pointer(T* ptr) {
    // This is an internal helper. Direct use is discouraged.
    // It's used by constructors assuming proper ref counting is handled externally or initially.
    // For example, sp(T* other) calls this.
    m_ptr = ptr;
    // Note: incStrong is typically called by the caller of set_pointer (e.g., constructor)
}

template<typename T>
/*static*/ sp<T> sp<T>::fromExisting(T* ptr) {
    sp<T> result; // Default constructs to nullptr
    if (ptr) {
        ptr->incStrong(&result); // Increment ref count before assigning
        result.m_ptr = ptr;
    }
    return result;
}


template<typename T>
template<typename... Args>
/*static*/ sp<T> sp<T>::make(Args&&... args) {
    // TODO(b/175724971): AOSP uses a more complex make that might involve custom allocators
    // or specific RefBase behavior. This is a simplified version.
    // It assumes T inherits from RefBase.
    // We cannot directly use `new T(std::forward<Args>(args)...)` and then `sp(raw_ptr)`
    // because if the constructor of T itself creates an sp to itself, the ref count
    // could be problematic.
    // A common pattern is to construct, then call onFirstRef manually or ensure
    // the constructor doesn't interact with sp/wp of itself.
    // For now, a simple new and wrap, assuming T's constructor is well-behaved.
    T* raw_ptr = new T(std::forward<Args>(args)...);
    // The sp constructor `sp(T* other)` will call incStrong.
    // If T's constructor calls onFirstRef (as RefBase does if it's the first ref),
    // this should be okay.
    return sp<T>(raw_ptr);
}

template <typename T>
template <typename U>
/*static*/ sp<T> sp<T>::cast(const sp<U>& uPtr) {
    // This performs a static_cast on the raw pointer and returns a new sp.
    // It relies on the sp(T*) constructor to handle incStrong.
    // If the cast is invalid, this will result in a compile-time error
    // or runtime issues if the types are not compatible.
    T* ptrT = static_cast<T*>(uPtr.get());
    return sp<T>(ptrT); // This will incStrong on ptrT if not null
}


template <typename T>
inline TextOutput& operator<<(TextOutput& to, const sp<T>& val) {
    // AOSP uses printStrongPointer(to, val.get())
    // This is consistent.
    return printStrongPointer(to, val.get());
}

} // namespace android

// ---------------------------------------------------------------------------
// Support for std::hash
namespace std {
    template<typename T> struct hash<android::sp<T>> {
        size_t operator()(const android::sp<T>& p) const {
            return std::hash<T*>()(p.get());
        }
    };
} // namespace std
// ---------------------------------------------------------------------------


#endif // ANDROID_STRONG_POINTER_H
