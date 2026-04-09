// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __SIGNALSAFETHREADMAP_H__
#define __SIGNALSAFETHREADMAP_H__

#if defined(TARGET_UNIX) && !defined(TARGET_WASM)

//  Insert a thread into the signal-safe map.
//  * osThread - The OS thread ID to insert.
//  * pThread - A pointer to the thread object to associate with the OS thread ID.
//  * return true if the insertion was successful, false otherwise (OOM).
bool InsertThreadIntoSignalSafeMap(size_t osThread, void* pThread);

// Remove a thread from the signal-safe map.
// * osThread - The OS thread ID to remove.
// * pThread - A pointer to the thread object associated with the OS thread ID.
void RemoveThreadFromSignalSafeMap(size_t osThread, void* pThread);

// Find a thread in the signal-safe map.
// * osThread - The OS thread ID to search for.
// * return - A pointer to the thread object associated with the OS thread ID, or NULL if not found.
void* FindThreadInSignalSafeMap(size_t osThread);

typedef void (*SignalSafeThreadMapCallback)(size_t osThread, void* pThread, void* context);

// Enumerate the current contents of the signal-safe map.
// The callback may observe a best-effort snapshot if other threads are attaching
// or detaching concurrently, but the traversal itself uses only atomic loads.
void EnumerateThreadsInSignalSafeMap(SignalSafeThreadMapCallback callback, void* context);

#endif // TARGET_UNIX && !TARGET_WASM

#endif // __SIGNALSAFETHREADMAP_H__
