//===- core/adt/ilist_node_base.h - Intrusive List Node Base -----*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LIBBASE_ADT_ILIST_NODE_BASE_H
#define LIBBASE_ADT_ILIST_NODE_BASE_H

#include "core/adt/PointerIntPair.h"

namespace libbase
{

namespace ilist_detail
{

template <class NodeBase, bool EnableSentinelTracking> class node_base_prevnext;

template <class NodeBase> class node_base_prevnext<NodeBase, false>
{
  NodeBase *Prev = nullptr;
  NodeBase *Next = nullptr;

public:
  void
  setPrev (NodeBase *Prev)
  {
    this->Prev = Prev;
  }
  void
  setNext (NodeBase *Next)
  {
    this->Next = Next;
  }
  NodeBase *
  getPrev () const
  {
    return Prev;
  }
  NodeBase *
  getNext () const
  {
    return Next;
  }

  bool
  isKnownSentinel () const
  {
    return false;
  }
  void
  initializeSentinel ()
  {
  }
};

template <class NodeBase> class node_base_prevnext<NodeBase, true>
{
  PointerIntPair<NodeBase *, 1> PrevAndSentinel;
  NodeBase *Next = nullptr;

public:
  void
  setPrev (NodeBase *Prev)
  {
    PrevAndSentinel.setPointer (Prev);
  }
  void
  setNext (NodeBase *Next)
  {
    this->Next = Next;
  }
  NodeBase *
  getPrev () const
  {
    return PrevAndSentinel.getPointer ();
  }
  NodeBase *
  getNext () const
  {
    return Next;
  }

  bool
  isSentinel () const
  {
    return PrevAndSentinel.getInt ();
  }
  bool
  isKnownSentinel () const
  {
    return isSentinel ();
  }
  void
  initializeSentinel ()
  {
    PrevAndSentinel.setInt (true);
  }
};

template <class ParentTy> class node_base_parent
{
  ParentTy *Parent = nullptr;

public:
  void
  setNodeBaseParent (ParentTy *Parent)
  {
    this->Parent = Parent;
  }
  inline const ParentTy *
  getNodeBaseParent () const
  {
    return Parent;
  }
  inline ParentTy *
  getNodeBaseParent ()
  {
    return Parent;
  }
};
template <> class node_base_parent<void>
{
};

} // end namespace ilist_detail

/// Base class for ilist nodes.
///
/// Optionally tracks whether this node is the sentinel.
template <bool EnableSentinelTracking, class ParentTy>
class ilist_node_base : public ilist_detail::node_base_prevnext<
                            ilist_node_base<EnableSentinelTracking, ParentTy>,
                            EnableSentinelTracking>,
                        public ilist_detail::node_base_parent<ParentTy>
{
};

// Specialization implemented in the core LLVM library.
template class LIBBASE_ABI ilist_node_base<true, void>;

} // end namespace libbase

#endif // LIBBASE_ADT_ILIST_NODE_BASE_H
/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.

 RCC-Project is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free
 Software Foundation; either version 3 of the License, or (at your option) any
 later version.

 RCC-Project is distributed in the hope that it will be useful, but WITHOUT ANY
 WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 A PARTICULAR PURPOSE. See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along with
 RCC-Project; see the file COPYING. If not see
 <http://www.gnu.org/licenses/>.
*/
