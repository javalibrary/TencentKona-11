/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef SHARE_GC_Z_ZHEAP_INLINE_HPP
#define SHARE_GC_Z_ZHEAP_INLINE_HPP

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zHeap.hpp"
#include "gc/z/zMark.inline.hpp"
#include "gc/z/zOop.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageTable.inline.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "utilities/debug.hpp"

inline ZHeap* ZHeap::heap() {
  assert(_heap != NULL, "Not initialized");
  return _heap;
}

inline ReferenceDiscoverer* ZHeap::reference_discoverer() {
  return &_reference_processor;
}

inline bool ZHeap::is_relocating(uintptr_t addr) const {
  return _pagetable.is_relocating(addr);
}

inline bool ZHeap::is_object_live(uintptr_t addr) const {
  ZPage* page = _pagetable.get(addr);
  return page->is_object_live(addr);
}

inline bool ZHeap::is_object_strongly_live(uintptr_t addr) const {
  ZPage* page = _pagetable.get(addr);
  return page->is_object_strongly_live(addr);
}

template <bool gc_thread, bool finalizable, bool publish>
inline void ZHeap::mark_object(uintptr_t addr) {
  assert(ZGlobalPhase == ZPhaseMark, "Mark not allowed");
  _mark.mark_object<gc_thread, finalizable, publish>(addr);
}

inline uintptr_t ZHeap::alloc_tlab(size_t size) {
  guarantee(size <= max_tlab_size(), "TLAB too large");
  return _object_allocator.alloc_object(size);
}

inline uintptr_t ZHeap::alloc_object(size_t size) {
  uintptr_t addr = _object_allocator.alloc_object(size);
  assert(ZAddress::is_good_or_null(addr), "Bad address");

  if (addr == 0) {
    out_of_memory();
  }

  return addr;
}

inline uintptr_t ZHeap::alloc_object_for_relocation(size_t size) {
  uintptr_t addr = _object_allocator.alloc_object_for_relocation(size);
  assert(ZAddress::is_good_or_null(addr), "Bad address");
  return addr;
}

inline void ZHeap::undo_alloc_object_for_relocation(uintptr_t addr, size_t size) {
  ZPage* const page = _pagetable.get(addr);
  _object_allocator.undo_alloc_object_for_relocation(page, addr, size);
}

inline bool ZHeap::is_alloc_stalled() const {
  return _page_allocator.is_alloc_stalled();
}

inline void ZHeap::check_out_of_memory() {
  _page_allocator.check_out_of_memory();
}

inline bool ZHeap::is_oop(oop object) const {
  return ZOop::is_good(object);
}

#endif // SHARE_GC_Z_ZHEAP_INLINE_HPP
