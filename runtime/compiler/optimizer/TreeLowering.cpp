/*******************************************************************************
 * Copyright (c) 2021, 2021 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "optimizer/TreeLowering.hpp"

#include "compile/Compilation.hpp"
#include "compile/SymbolReferenceTable.hpp"
#include "il/Block.hpp"
#include "il/Block_inlines.hpp"
#include "infra/ILWalk.hpp"

const char *
TR::TreeLowering::optDetailString() const throw()
   {
   return "O^O TREE LOWERING: ";
   }

int32_t
TR::TreeLowering::perform()
   {
   if (!TR::Compiler->om.areValueTypesEnabled())
      {
      return 0;
      }

   TR::ResolvedMethodSymbol* methodSymbol = comp()->getMethodSymbol();
   for (TR::PreorderNodeIterator nodeIter(methodSymbol->getFirstTreeTop(), comp()); nodeIter != NULL ; ++nodeIter)
      {
      TR::Node* node = nodeIter.currentNode();
      TR::TreeTop* tt = nodeIter.currentTree();

      if (TR::Compiler->om.areValueTypesEnabled())
         {
         lowerValueTypeOperations(node, tt);
         }
      }

   return 0;
   }

void
TR::TreeLowering::moveNodeToEndOfBlock(TR::Block* const block, TR::TreeTop* const tt, TR::Node* const node)
   {
   TR::Compilation* comp = self()->comp();
   TR::TreeTop* blockExit = block->getExit();
   TR::TreeTop* iterTT = tt->getNextTreeTop();

   if (iterTT != blockExit)
      {
      if (trace())
         {
         traceMsg(comp, "Moving treetop containing node n%dn [%p] for acmp helper call to end of prevBlock in preparation of final block split\n", tt->getNode()->getGlobalIndex(), tt->getNode());
         }

      // Remove TreeTop for call node, and gather it and the treetops for stores that
      // resulted from un-commoning in a TreeTop chain from tt to lastTTForCallBlock
      tt->unlink(false);
      TR::TreeTop* lastTTForCallBlock = tt;

      while (iterTT != blockExit)
         {
         TR::TreeTop* nextTT = iterTT->getNextTreeTop();
         TR::ILOpCodes op = iterTT->getNode()->getOpCodeValue();

         if ((op == TR::iRegStore || op == TR::istore) && iterTT->getNode()->getFirstChild() == node)
            {
            if (trace())
               {
               traceMsg(comp, "Moving treetop containing node n%dn [%p] for store of acmp helper result to end of prevBlock in preparation of final block split\n", iterTT->getNode()->getGlobalIndex(), iterTT->getNode());
               }

            // Remove store node from prevBlock temporarily
            iterTT->unlink(false);
            lastTTForCallBlock->join(iterTT);
            lastTTForCallBlock = iterTT;
            }

         iterTT = nextTT;
         }

      // Move the treetops that were gathered for the call and any stores of the
      // result to the end of the block in preparation for the split of the call block
      blockExit->getPrevTreeTop()->join(tt);
      lastTTForCallBlock->join(blockExit);
      }
   }

/**
 * @brief Perform lowering related to Valhalla value types
 *
 */
void
TR::TreeLowering::lowerValueTypeOperations(TR::Node* node, TR::TreeTop* tt)
   {
   TR::SymbolReferenceTable * symRefTab = comp()->getSymRefTab();

   if (node->getOpCode().isCall() &&
         symRefTab->isNonHelper(node->getSymbolReference(), TR::SymbolReferenceTable::objectEqualityComparisonSymbol))
      {
      // turn the non-helper call into a VM helper call
      node->setSymbolReference(symRefTab->findOrCreateAcmpHelperSymbolRef());
      static const bool disableAcmpFastPath =  NULL != feGetEnv("TR_DisableAcmpFastpath");
      if (!disableAcmpFastPath)
         {
         fastpathAcmpHelper(node, tt);
         }
      }
   else if (node->getOpCodeValue() == TR::ArrayStoreCHK)
      {
      lowerArrayStoreCHK(node, tt);
      }
   }

/**
 * @brief Copy register dependencies between GlRegDeps node at exit points.
 *
 * This function is only intended to work with GlRegDeps nodes for exit points,
 * (i.e. BBEnd, branch, or jump nodes) within the same extended basic block.
 *
 * Register dependencies are copied "logically", meaning that the actual node
 * used to represent a dependency won't necessarily be copied. If the reg dep
 * is represented by a PassThrough, then the node itself is copied and its
 * child is commoned (so it's lifetime is exteneded; note that in correctly-formed
 * IL, the child must also be the child of a reg store in the containing block).
 * Otherwise, the dependency must be represented by a reg load, which must have
 * come from the GlRegDeps node at the entry point and *must* be commoned
 * (so it won't get copied).
 *
 * In addition, this function allows *one* register dependency to be changed
 * (substituted). That is, if a register dependency is found under `sourceNode`
 * for the same register that is set on `subsituteNode`, then `substituteNode`
 * will be used instead of the dependency from `sourceNode`. Note that the
 * reference of of `substituteNode` is incremented if/when it gets added. If
 * `substituteNode` is NULL the no substitution will be attempted.
 *
 * @param targetNode is the GlRegDeps node that reg deps are copied to
 * @param sourceNode is the GlRegDeps node that reg deps are copied from
 * @param substituteNode is the reg dep node to substitute if a matching register is found in `sourceNode` (NULL if none)
 */
void
copyExitRegDepsAndSubstitue(TR::Node* const targetNode, TR::Node* const sourceNode, TR::Node* const substituteNode)
   {
   for (int i = 0; i < sourceNode->getNumChildren(); ++i)
      {
      TR::Node* child = sourceNode->getChild(i);
      if (substituteNode
          && child->getLowGlobalRegisterNumber() == substituteNode->getLowGlobalRegisterNumber()
          && child->getHighGlobalRegisterNumber() == substituteNode->getHighGlobalRegisterNumber())
         targetNode->setAndIncChild(i, substituteNode);
      else if (child->getOpCodeValue() == TR::PassThrough)
         {
         // PassThrough nodes cannot be commoned because doing so does not
         // actually anchor the child, causing it's lifetime to not be extended
         child = TR::Node::copy(child);
         if (child->getFirstChild())
            {
            child->getFirstChild()->incReferenceCount();
            }
         child->setReferenceCount(1);
         targetNode->setChild(i, child);
         }
      else
         {
         // all other nodes must be commoned as they won't get evluated otherwise
         targetNode->setAndIncChild(i, child);
         }
      }
   }

/**
 * @brief Add a GlRegDeps node to a branch by copying some other GlRegDeps.
 *
 * Given branch node, adds a GlRegDeps node by copying the dependencies from
 * a different GlRegDeps. This function allows *one* register dependency to
 * be changed (substituted). See `copyExitRegDepsAndSubstitue()` for details.
 *
 * Note that the branch node is assumed to *not* have a GlRegDeps node already.
 *
 * Returns a pointer to the newly created GlRegDeps. This is can be particularly
 * useful to have whe doing a substitution (e.g. for chaining calls).
 *
 * If the source GlRegDeps is NULL, then nothing is done and NULL is returned.
 *
 * @param branchNode is the branch node the GlRegDeps will be added to
 * @param sourceGlRegDepsNode is the GlRegDeps node used to copy the reg deps from
 * @param substituteNode is the reg dep node to be subsituted (NULL if none)
 * @return TR::Node* the newly created GlRegDeps or NULL if `sourceGelRegDepsNode` was NULL
 */
TR::Node*
copyBranchGlRegDepsAndSubstitute(TR::Node* const branchNode, TR::Node* const sourceGlRegDepsNode, TR::Node* const substituteNode)
   {
   TR::Node* glRegDepsCopy = NULL;
   if (sourceGlRegDepsNode != NULL)
      {
      glRegDepsCopy = TR::Node::create(TR::GlRegDeps, sourceGlRegDepsNode->getNumChildren());
      copyExitRegDepsAndSubstitue(glRegDepsCopy, sourceGlRegDepsNode, substituteNode);
      branchNode->addChildren(&glRegDepsCopy, 1);
      }
   return glRegDepsCopy;
   }

TR::Block*
TR::TreeLowering::splitForFastpath(TR::Block* const block, TR::TreeTop* const splitPoint, TR::Block* const targetBlock)
   {
   TR::CFG* const cfg = self()->comp()->getFlowGraph();
   TR::Block* const newBlock = block->split(splitPoint, cfg);
   newBlock->setIsExtensionOfPreviousBlock(true);
   cfg->addEdge(block, targetBlock);
   return newBlock;
   }

/**
 * @brief Add checks to skip (fast-path) acmpHelper call
 *
 * @details
 *
 * This transformation adds checks for the cases where the acmp can be performed
 * without calling the VM helper. The transformed Trees represent the following operation:
 *
 * 1. If the address of lhs and rhs are the same, produce an eq (true) result
 *    and skip the call (note the two objects must be the same regardless of
 *    whether they are value types are reference types)
 * 2. Otherwise, do VM helper call
 *
 * The transformation looks as follows:
 *
 *  +----------------------+
 *  |ttprev                |
 *  |treetop               |
 *  |  icall acmpHelper    |
 *  |    aload lhs         |
 *  |    aload rhs         |
 *  |ificmpeq --> ...      |
 *  |  ==> icall           |
 *  |  iconst 0            |
 *  |BBEnd                 |
 *  +----------------------+
 *
 *  ...becomes...
 *
 *
 * +------------------------------+
 * |ttprev                        |
 * |iRegStore x                   |
 * |  iconst 1                    |
 * |ifacmpeq  +->---------------------------+
 * |  aload lhs                   |         |
 * |  aload rhs                   |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 1            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |iRegStore x                   |         |
 * |  iconst 0                    |         |
 * |ifacmpeq +->----------------------------+
 * |  aload lhs                   |         |
 * |  aconst 0                    |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |ifacmpeq +------------------------------+
 * |  aload rhs                   |         |
 * |  ==> aconst 0                |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |ifacmpeq +->----------------------------+
 * |  iand                        |         |
 * |    iloadi ClassFlags         |         |
 * |      aloadi J9Class          |         |
 * |        aload lhs             |         |
 * |    iconst J9ClassIsValueType |         |
 * |  iconst 0                    |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |ifacmpeq +->----------------------------+
 * |  iand                        |         |
 * |    iloadi ClassFlags         |         |
 * |      aloadi J9Class          |         |
 * |        aload rhs             |         |
 * |    iconst J9ClassIsValueType |         |
 * |  iconst 0                    |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |iRegStore x                   |         |
 * |  icall acmpHelper            |         |
 * |    aload lhs                 |         |
 * |    aload rhs                 |         |
 * |BBEnd                         |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> icall acmpHelper    |         |
 * |    PassThrough ...           |         |
 * +-----+------------------------+         |
 *       |                                  |
 *       +----------------------------------+
 *       |
 *       v
 * +-----+-----------+
 * |BBStart          |
 * |ificmpeq +-> ... +
 * |  iRegLoad x     |
 * |  iconst 0       |
 * |BBEnd            |
 * +-----------------+
 *
 * Any GlRegDeps on the extension block are created by OMR::Block::splitPostGRA
 * while those on the ifacmpeq at the end of the first block are copies of those,
 * with the exception of any register (x, above) holding the result of the compare
 *
 * @param node is the current node in the tree walk
 * @param tt is the treetop at the root of the tree ancoring the current node
 *
 */
void
TR::TreeLowering::fastpathAcmpHelper(TR::Node * const node, TR::TreeTop * const tt)
   {
   TR::Compilation* comp = self()->comp();
   TR::CFG* cfg = comp->getFlowGraph();
   cfg->invalidateStructure();

   if (!performTransformation(comp, "%sPreparing for post-GRA block split by anchoring helper call and arguments\n", optDetailString()))
      return;

   // Anchor call node after split point to ensure the returned value goes into
   // either a temp or a global register.
   auto* const anchoredCallTT = TR::TreeTop::create(comp, tt, TR::Node::create(TR::treetop, 1, node));
   if (trace())
      traceMsg(comp, "Anchoring call node under treetop n%un (0x%p)\n", anchoredCallTT->getNode()->getGlobalIndex(), anchoredCallTT->getNode());

   // Anchor the call arguments just before the call. This ensures the values are
   // live before the call so that we can propagate their values in global registers if needed.
   auto* const anchoredCallArg1TT = TR::TreeTop::create(comp, tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, node->getFirstChild()));
   auto* const anchoredCallArg2TT = TR::TreeTop::create(comp, tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, node->getSecondChild()));
   if (trace())
      {
      traceMsg(comp, "Anchoring call arguments n%un and n%un under treetops n%un and n%un\n",
         node->getFirstChild()->getGlobalIndex(), node->getSecondChild()->getGlobalIndex(), anchoredCallArg1TT->getNode()->getGlobalIndex(), anchoredCallArg2TT->getNode()->getGlobalIndex());
      }

   // Split the block at the call TreeTop so that the new block created
   // after the call can become a merge point for all the fastpaths.
   TR::Block* callBlock = tt->getEnclosingBlock();
   if (!performTransformation(comp, "%sSplitting block_%d at TreeTop [0x%p], which holds helper call node n%un\n", optDetailString(), callBlock->getNumber(), tt, node->getGlobalIndex()))
      return;
   TR::Block* targetBlock = callBlock->splitPostGRA(tt->getNextTreeTop(), cfg, true, NULL);
   if (trace())
      traceMsg(comp, "Call node n%un is in block %d, targetBlock is %d\n", node->getGlobalIndex(), callBlock->getNumber(), targetBlock->getNumber());

   // As the block is split after the helper call node, it is possible that as part of un-commoning
   // code to store nodes into registers or temp-slots is appended to the original block by the call
   // to splitPostGRA above.  Move the acmp helper call treetop to the end of prevBlock, along with
   // any stores resulting from un-commoning of the nodes in the helper call tree so that it can be
   // split into its own call block.
   moveNodeToEndOfBlock(callBlock, tt, node);

   if (!performTransformation(comp, "%sInserting fastpath for lhs == rhs\n", optDetailString()))
      return;

   // Insert store of constant 1 as the result of the fastpath.
   // The value must go wherever the value returned by the helper call goes
   // so that the code in the target block (merge point) picks up the constant
   // if the branch is taken. Use the TreeTop previously inserted to anchor the
   // call to figure out where the return value of the call is being put.
   TR::Node* anchoredNode = anchoredCallTT->getNode()->getFirstChild(); // call node is under a treetop node
   if (trace())
      traceMsg(comp, "Anchored call has been transformed into %s node n%un\n", anchoredNode->getOpCode().getName(), anchoredNode->getGlobalIndex());
   auto* const1Node = TR::Node::iconst(1);
   TR::Node* storeNode = NULL;
   TR::Node* regDepForStoreNode = NULL; // this is the reg dep for the store if one is needed
   if (anchoredNode->getOpCodeValue() == TR::iRegLoad)
      {
      if (trace())
         traceMsg(comp, "Storing constant 1 in register %s\n", comp->getDebug()->getGlobalRegisterName(anchoredNode->getGlobalRegisterNumber()));
      auto const globalRegNum = anchoredNode->getGlobalRegisterNumber();
      storeNode = TR::Node::create(TR::iRegStore, 1, const1Node);
      storeNode->setGlobalRegisterNumber(globalRegNum);
      // Since the result is in a global register, we're going to need a PassThrough
      // on the exit point GlRegDeps.
      regDepForStoreNode = TR::Node::create(TR::PassThrough, 1, const1Node);
      regDepForStoreNode->setGlobalRegisterNumber(globalRegNum);
      }
   else if (anchoredNode->getOpCodeValue() == TR::iload)
      {
      if (trace())
         traceMsg(comp, "Storing constant 1 to symref %d (%s)\n", anchoredNode->getSymbolReference()->getReferenceNumber(), anchoredNode->getSymbolReference()->getName(comp->getDebug()));
      storeNode = TR::Node::create(TR::istore, 1, const1Node);
      storeNode->setSymbolReference(anchoredNode->getSymbolReference());
      }
   else
      TR_ASSERT_FATAL_WITH_NODE(anchoredNode, false, "Anchored call has been turned into unexpected opcode\n");
   tt->insertBefore(TR::TreeTop::create(comp, storeNode));

   // If the BBEnd of the block containing the call has a GlRegDeps node,
   // a matching GlRegDeps node will be needed for all the branches. The
   // fallthrough of the call block and the branch targets will be the
   // same block. So, all register dependencies will be mostly the same.
   // `exitGlRegDeps` is intended to point to the "reference" node used to
   // create the GlRegDeps for each consecutive branch.
   TR::Node* exitGlRegDeps = NULL;
   if (callBlock->getExit()->getNode()->getNumChildren() > 0)
      {
      exitGlRegDeps = callBlock->getExit()->getNode()->getFirstChild();
      }

   // Insert fastpath for lhs == rhs (reference comparison), taking care to set the
   // proper register dependencies by copying them from the BBEnd of the call block
   // (through `exitGlRegDeps`) when needed.
   auto* ifacmpeqNode = TR::Node::createif(TR::ifacmpeq, anchoredCallArg1TT->getNode()->getFirstChild(), anchoredCallArg2TT->getNode()->getFirstChild(), targetBlock->getEntry());
   exitGlRegDeps = copyBranchGlRegDepsAndSubstitute(ifacmpeqNode, exitGlRegDeps, regDepForStoreNode);
   tt->insertBefore(TR::TreeTop::create(comp, ifacmpeqNode));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   if (trace())
      traceMsg(comp, "Added check node n%un; call node is now in block_%d\n", ifacmpeqNode->getGlobalIndex(), callBlock->getNumber());

   if (!performTransformation(comp, "%sInserting fastpath for lhs == NULL\n", optDetailString()))
      return;

   // Create store of 0 as fastpath result by duplicate the node used to store
   // the constant 1. Also duplicate the corresponding regdep if needed.
   storeNode = storeNode->duplicateTree(true);
   storeNode->getFirstChild()->setInt(0);
   tt->insertBefore(TR::TreeTop::create(comp, storeNode));
   if (regDepForStoreNode != NULL)
      {
      regDepForStoreNode = TR::Node::copy(regDepForStoreNode);
      regDepForStoreNode->setReferenceCount(0);
      regDepForStoreNode->setAndIncChild(0, storeNode->getFirstChild());
      }

   // Using a similar strategy as above, insert check for lhs == NULL.
   auto* const nullConst = TR::Node::aconst(0);
   auto* const checkLhsNull = TR::Node::createif(TR::ifacmpeq, anchoredCallArg1TT->getNode()->getFirstChild(), nullConst, targetBlock->getEntry());
   exitGlRegDeps = copyBranchGlRegDepsAndSubstitute(checkLhsNull, exitGlRegDeps, regDepForStoreNode);
   tt->insertBefore(TR::TreeTop::create(comp, checkLhsNull));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   if (trace())
      traceMsg(comp, "Added check node n%un; call node is now in block_%d\n", checkLhsNull->getGlobalIndex(), callBlock->getNumber());

   if (!performTransformation(comp, "%sInserting fastpath for rhs == NULL\n", optDetailString()))
      return;

   auto* const checkRhsNull = TR::Node::createif(TR::ifacmpeq, anchoredCallArg2TT->getNode()->getFirstChild(), nullConst, targetBlock->getEntry());
   copyBranchGlRegDepsAndSubstitute(checkRhsNull, exitGlRegDeps, NULL);
   tt->insertBefore(TR::TreeTop::create(comp, checkRhsNull));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   if (trace())
      traceMsg(comp, "Added check node n%un; call node is now in block_%d\n", checkRhsNull->getGlobalIndex(), callBlock->getNumber());

   if (!performTransformation(comp, "%sInserting fastpath for lhs is VT\n", optDetailString()))
      return;

   auto* const vftSymRef = comp->getSymRefTab()->findOrCreateVftSymbolRef();
   auto* const classFlagsSymRef = comp->getSymRefTab()->findOrCreateClassFlagsSymbolRef();
   auto* const j9ClassIsVTFlag = TR::Node::iconst(node, J9ClassIsValueType);

   auto* const lhsVft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredCallArg1TT->getNode()->getFirstChild(), vftSymRef);
   auto* const lhsClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, lhsVft, classFlagsSymRef);
   auto* const isLhsValueType = TR::Node::create(node, TR::iand, 2, lhsClassFlags, j9ClassIsVTFlag);
   auto* const checkLhsIsVT = TR::Node::createif(TR::ificmpeq, isLhsValueType, storeNode->getFirstChild(), targetBlock->getEntry());
   copyBranchGlRegDepsAndSubstitute(checkLhsIsVT, exitGlRegDeps, NULL);
   tt->insertBefore(TR::TreeTop::create(comp, checkLhsIsVT));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);

   if (!performTransformation(comp, "%sInserting fastpath for rhs is VT\n", optDetailString()))
      return;

   auto* const rhsVft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredCallArg2TT->getNode()->getFirstChild(), vftSymRef);
   auto* const rhsClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, rhsVft, classFlagsSymRef);
   auto* const isRhsValueType = TR::Node::create(node, TR::iand, 2, rhsClassFlags, j9ClassIsVTFlag);
   auto* const checkRhsIsVT = TR::Node::createif(TR::ificmpeq, isRhsValueType, storeNode->getFirstChild(), targetBlock->getEntry());
   copyBranchGlRegDepsAndSubstitute(checkRhsIsVT, exitGlRegDeps, NULL);
   tt->insertBefore(TR::TreeTop::create(comp, checkRhsIsVT));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   }

/**
 * If value types are enabled, and the value that is being assigned to the array
 * element might be a null reference, lower the ArrayStoreCHK by splitting the
 * block before the ArrayStoreCHK, and inserting a NULLCHK guarded by a check
 * of whether the array's component type is a value type.
 *
 * @param node is the current node in the tree walk
 * @param tt is the treetop at the root of the tree ancoring the current node
 */
void
TR::TreeLowering::lowerArrayStoreCHK(TR::Node *node, TR::TreeTop *tt)
   {
   // Pattern match the ArrayStoreCHK operands to get the source of the assignment
   // (sourceChild) and the array to which an element will have a value assigned (destChild)
   TR::Node *firstChild = node->getFirstChild();

   TR::Node *sourceChild = firstChild->getSecondChild();
   TR::Node *destChild = firstChild->getChild(2);

   // Only need to lower if it is possible that the value is a null reference
   if (!sourceChild->isNonNull())
      {
      TR::CFG * cfg = comp()->getFlowGraph();
      cfg->invalidateStructure();

      TR::Block *prevBlock = tt->getEnclosingBlock();

      performTransformation(comp(), "%sTransforming ArrayStoreCHK n%dn [%p] by splitting block block_%d, and inserting a NULLCHK guarded with a check of whether the component type of the array is a value type\n", optDetailString(), node->getGlobalIndex(), node, prevBlock->getNumber());

      // Anchor the node containing the source of the array element
      // assignment and the node that contains the destination array
      // to ensure they are available for the ificmpeq and NULLCHK
      TR::TreeTop *anchoredArrayTT = TR::TreeTop::create(comp(), tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, destChild));
      TR::TreeTop *anchoredSourceTT = TR::TreeTop::create(comp(), anchoredArrayTT, TR::Node::create(TR::treetop, 1, sourceChild));

      // Transform
      //   +--------------------------------+
      //   | ttprev                         |
      //   | ArrayStoreCHK                  |
      //   |   astorei/awrtbari             |
      //   |     aladd                      |
      //   |       <array-reference>        |
      //   |       index-offset-calculation |
      //   |     <value-reference>          |
      //   +--------------------------------+
      //
      // into
      //   +--------------------------------+
      //   | treetop                        |
      //   |   <array-reference>            |
      //   | treetop                        |
      //   |   <value-reference>            |
      //   | ificmpeq  -->------------------*---------+
      //   |   iand                         |         |
      //   |     iloadi <isClassFlags>      |         |
      //   |       aloadi <componentClass>  |         |
      //   |         aloadi <vft-symbol>    |         |
      //   |           <array-reference>    |         |
      //   |     iconst J9ClassIsValueType  |         |
      //   |   iconst 0                     |         |
      //   | BBEnd                          |         |
      //   +--------------------------------+         |
      //   | BBStart (Extension)            |         |
      //   | NULLCHK                        |         |
      //   |   Passthrough                  |         |
      //   |     <value-reference>          |         |
      //   | BBEnd                          |         |
      //   +--------------------------------+         |
      //                   |                          |
      //                   +--------------------------+
      //                   |
      //                   v
      //   +--------------------------------+
      //   | BBStart                        |
      //   | ArrayStoreCHK                  |
      //   |   astorei/awrtbari             |
      //   |     aladd                      |
      //   |       aload <array>            |
      //   |       index-offset-calculation |
      //   |     aload <value>              |
      //   +--------------------------------+
      //
      TR::SymbolReference *vftSymRef = comp()->getSymRefTab()->findOrCreateVftSymbolRef();
      TR::SymbolReference *arrayCompSymRef = comp()->getSymRefTab()->findOrCreateArrayComponentTypeSymbolRef();
      TR::SymbolReference *classFlagsSymRef = comp()->getSymRefTab()->findOrCreateClassFlagsSymbolRef();

      TR::Node *vft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredArrayTT->getNode()->getFirstChild(), vftSymRef);
      TR::Node *arrayCompClass = TR::Node::createWithSymRef(node, TR::aloadi, 1, vft, arrayCompSymRef);
      TR::Node *loadClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, arrayCompClass, classFlagsSymRef);
      TR::Node *isValueTypeNode = TR::Node::create(node, TR::iand, 2, loadClassFlags, TR::Node::iconst(node, J9ClassIsValueType));

      TR::Node *ifNode = TR::Node::createif(TR::ificmpeq, isValueTypeNode, TR::Node::iconst(node, 0));
      ifNode->copyByteCodeInfo(node);

      TR::Node *passThru  = TR::Node::create(node, TR::PassThrough, 1, sourceChild);
      TR::ResolvedMethodSymbol *currentMethod = comp()->getMethodSymbol();

      TR::Block *arrayStoreCheckBlock = prevBlock->splitPostGRA(tt, cfg);

      ifNode->setBranchDestination(arrayStoreCheckBlock->getEntry());

      // Copy register dependencies from the end of the block split before the
      // ArrayStoreCHK to the ificmpeq that's being added to the end of that block
      if (prevBlock->getExit()->getNode()->getNumChildren() != 0)
         {
         TR::Node *blkDeps = prevBlock->getExit()->getNode()->getFirstChild();
         TR::Node *ifDeps = TR::Node::create(blkDeps, TR::GlRegDeps);

         for (int i = 0; i < blkDeps->getNumChildren(); i++)
            {
            TR::Node *regDep = blkDeps->getChild(i);

            if (regDep->getOpCodeValue() == TR::PassThrough)
               {
               TR::Node *orig= regDep;
               regDep = TR::Node::create(orig, TR::PassThrough, 1, orig->getFirstChild());
               regDep->setLowGlobalRegisterNumber(orig->getLowGlobalRegisterNumber());
               regDep->setHighGlobalRegisterNumber(orig->getHighGlobalRegisterNumber());
               }

            ifDeps->addChildren(&regDep, 1);
            }

         ifNode->addChildren(&ifDeps, 1);
         }

      prevBlock->append(TR::TreeTop::create(comp(), ifNode));

      TR::Node *nullCheck = TR::Node::createWithSymRef(node, TR::NULLCHK, 1, passThru,
                               comp()->getSymRefTab()->findOrCreateNullCheckSymbolRef(currentMethod));
      TR::TreeTop *nullCheckTT = prevBlock->append(TR::TreeTop::create(comp(), nullCheck));

      TR::Block *nullCheckBlock = prevBlock->split(nullCheckTT, cfg);

      nullCheckBlock->setIsExtensionOfPreviousBlock(true);

      cfg->addEdge(prevBlock, arrayStoreCheckBlock);
      }
   }
