/*
 * Copyright (c) 2017, 2019, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/protectionDomainCache.hpp"
#include "classfile/systemDictionary.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "oops/weakHandle.inline.hpp"
#include "utilities/hashtable.inline.hpp"

unsigned int ProtectionDomainCacheTable::compute_hash(Handle protection_domain) {
  // Identity hash can safepoint, so keep protection domain in a Handle.
  return (unsigned int)(protection_domain->identity_hash());
}

int ProtectionDomainCacheTable::index_for(Handle protection_domain) {
  return hash_to_index(compute_hash(protection_domain));
}

ProtectionDomainCacheTable::ProtectionDomainCacheTable(int table_size)
  : Hashtable<ClassLoaderWeakHandle, mtClass>(table_size, sizeof(ProtectionDomainCacheEntry))
{   _dead_entries = false;
    _total_oops_removed = 0;
}

void ProtectionDomainCacheTable::trigger_cleanup() {
  MutexLockerEx ml(Service_lock, Mutex::_no_safepoint_check_flag);
  _dead_entries = true;
  Service_lock->notify_all();
}

class CleanProtectionDomainEntries : public CLDClosure {
  void do_cld(ClassLoaderData* data) {
    Dictionary* dictionary = data->dictionary();
    if (dictionary != NULL) {
      dictionary->clean_cached_protection_domains();
    }
  }
};

void ProtectionDomainCacheTable::unlink() {
  {
    // First clean cached pd lists in loaded CLDs
    // It's unlikely, but some loaded classes in a dictionary might
    // point to a protection_domain that has been unloaded.
    // The dictionary pd_set points at entries in the ProtectionDomainCacheTable.
    MutexLocker ml(ClassLoaderDataGraph_lock);
    MutexLocker mldict(SystemDictionary_lock);  // need both.
    CleanProtectionDomainEntries clean;
    ClassLoaderDataGraph::loaded_cld_do(&clean);
  }

  MutexLocker ml(SystemDictionary_lock);
  int oops_removed = 0;
  for (int i = 0; i < table_size(); ++i) {
    ProtectionDomainCacheEntry** p = bucket_addr(i);
    ProtectionDomainCacheEntry* entry = bucket(i);
    while (entry != NULL) {
      oop pd = entry->object_no_keepalive();
      if (pd != NULL) {
        p = entry->next_addr();
      } else {
        oops_removed++;
        LogTarget(Debug, protectiondomain, table) lt;
        if (lt.is_enabled()) {
          LogStream ls(lt);
          ls.print_cr("protection domain unlinked at %d", i);
        }
        entry->literal().release();
        *p = entry->next();
        free_entry(entry);
      }
      entry = *p;
    }
  }
  _total_oops_removed += oops_removed;
  _dead_entries = false;
}

void ProtectionDomainCacheTable::print_on(outputStream* st) const {
  assert_locked_or_safepoint(SystemDictionary_lock);
  st->print_cr("Protection domain cache table (table_size=%d, classes=%d)",
               table_size(), number_of_entries());
  for (int index = 0; index < table_size(); index++) {
    for (ProtectionDomainCacheEntry* probe = bucket(index);
                                     probe != NULL;
                                     probe = probe->next()) {
      st->print_cr("%4d: protection_domain: " PTR_FORMAT, index, p2i(probe->object_no_keepalive()));
    }
  }
}

void ProtectionDomainCacheTable::verify() {
  verify_table<ProtectionDomainCacheEntry>("Protection Domain Table");
}

oop ProtectionDomainCacheEntry::object() {
  return literal().resolve();
}

oop ProtectionDomainEntry::object() {
  return _pd_cache->object();
}

// The object_no_keepalive() call peeks at the phantomly reachable oop without
// keeping it alive. This is okay to do in the VM thread state if it is not
// leaked out to become strongly reachable.
oop ProtectionDomainCacheEntry::object_no_keepalive() {
  return literal().peek();
}

oop ProtectionDomainEntry::object_no_keepalive() {
  return _pd_cache->object_no_keepalive();
}

void ProtectionDomainCacheEntry::verify() {
  guarantee(object_no_keepalive() == NULL || oopDesc::is_oop(object_no_keepalive()), "must be an oop");
}

ProtectionDomainCacheEntry* ProtectionDomainCacheTable::get(Handle protection_domain) {
  unsigned int hash = compute_hash(protection_domain);
  int index = hash_to_index(hash);

  ProtectionDomainCacheEntry* entry = find_entry(index, protection_domain);
  if (entry == NULL) {
    entry = add_entry(index, hash, protection_domain);
  }
  // keep entry alive
  (void)entry->object();
  return entry;
}

ProtectionDomainCacheEntry* ProtectionDomainCacheTable::find_entry(int index, Handle protection_domain) {
  assert_locked_or_safepoint(SystemDictionary_lock);
  for (ProtectionDomainCacheEntry* e = bucket(index); e != NULL; e = e->next()) {
    if (e->object_no_keepalive() == protection_domain()) {
      return e;
    }
  }

  return NULL;
}

ProtectionDomainCacheEntry* ProtectionDomainCacheTable::add_entry(int index, unsigned int hash, Handle protection_domain) {
  assert_locked_or_safepoint(SystemDictionary_lock);
  assert(index == index_for(protection_domain), "incorrect index?");
  assert(find_entry(index, protection_domain) == NULL, "no double entry");

  LogTarget(Debug, protectiondomain, table) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    ls.print("protection domain added ");
    protection_domain->print_value_on(&ls);
    ls.cr();
  }
  ClassLoaderWeakHandle w = ClassLoaderWeakHandle::create(protection_domain);
  ProtectionDomainCacheEntry* p = new_entry(hash, w);
  Hashtable<ClassLoaderWeakHandle, mtClass>::add_entry(index, p);
  return p;
}
