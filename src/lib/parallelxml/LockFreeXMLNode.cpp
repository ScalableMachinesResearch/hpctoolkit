/*
 * Node.cpp
 *
 *  Created on: Dec 8, 2014
 *      Author: pat2
 */

#include "LockFreeXMLNode.hpp"
#include <cassert>
#include "Atomics.hpp"

LockFreeXMLNode::LockFreeXMLNode(const string& tagName, std::ostream* outstream)
: tag(tagName), out(outstream), attributeList(), m_ancestorCount(0), hidden(false) {
#ifndef __cilk
	attributeList = new std::list<Attribute>;
#endif
	firstChild = NULL;
	nextSibling = NULL;
	lastChild = NULL;
	previousSibling = NULL;
	parent = NULL;

	m_childCount = 0;
	attrStatus = OPEN;
}

LockFreeXMLNode::LockFreeXMLNode(LockFreeXMLNode* parent, const string& tagName) :
	tag(tagName), out(parent->out), attributeList(),
	firstChild(), lastChild(), nextSibling(), previousSibling(), parent(),
	attrStatus(OPEN), m_childCount(), hidden(false),
	m_ancestorCount(parent->m_ancestorCount + 1)
	{
		if (parent != NULL)
			parent->AddChild(this);
	}

LockFreeXMLNode::~LockFreeXMLNode() {
	// TODO Auto-generated destructor stub
}
LockFreeXMLNode* LockFreeXMLNode::createNode(const string& tag) {
	return new LockFreeXMLNode(tag, out);
}
LockFreeXMLNode::LockFreeXMLNode(const string& tagName, Outstream o)
: tag(tagName), attributeList(), out(o), hidden(false), m_ancestorCount(0)
{
#ifndef __cilk
	attributeList = new std::list<Attribute>;
#endif
	firstChild = NULL;
	nextSibling = NULL;
	previousSibling = NULL;
	lastChild = NULL;
	parent = NULL;

	m_childCount = 0;
	attrStatus = OPEN;
}

void LockFreeXMLNode::AddAttribute(Attribute a) {

	if (attrStatus == OPEN)
		attributeList.push_back(a);
	else
		throw 1;
}
void LockFreeXMLNode::FinalizeAttributes() {
	if (attrStatus == OPEN)
		attrStatus = FINALIZED;
}


void LockFreeXMLNode::write() {
	Write(false); // XML must have a single root element
}
void LockFreeXMLNode::Write(bool printSiblings) {
	out << "<" << tag;
	/*
#ifdef __cilk
	attributeList.get_value();
#else
	*attributeList;
#endif
*/
	for(AttributeList::iterator it = attributeList.begin(); it != attributeList.end(); ++it)
	{
		out << " " << it->name << "=\"" << it->value << "\"";
	}

	if (firstChild == NULL) {
		out << " />\n";
	} else {
		out << ">\n";
		cilk_spawn firstChild->Write(true);
		out << "</" << tag << ">\n";
	}

	// Could call nextSibling->Write(), but that would cause the
	// stack to grow with the width of the XML tree instead of just the depth.
	if (printSiblings) {
		LockFreeXMLNode* current = nextSibling;
		while (current != NULL) {
			cilk_spawn current->Write(false);
			current = current->nextSibling;
		}
	}
}

void LockFreeXMLNode::TransferAllMyChildrenTo(LockFreeXMLNode* newParent)
{
	// Expected use case: there may be races updating newParent but there won't be races updating this.
	// I'm not sure what it would mean to transfer all children and add children at the same time


	LockFreeXMLNode* iterator = firstChild;
	while (iterator != NULL)
	{ // if it weren't for the parent pointers, this could be O(1)...
		iterator->parent = newParent;
		iterator->m_ancestorCount = newParent->m_ancestorCount + 1;
	}

	LockFreeXMLNode* firstChildToTransfer;
	if (firstChild == NULL || newParent->firstChild != NULL)
		firstChildToTransfer = firstChild;
	else {
		// newParent doesn't have any children so let regular addChild deal with that special case
		firstChildToTransfer = firstChild->nextSibling;
		newParent->AddChild(firstChild);
		m_childCount--;
	}
	if (firstChildToTransfer == NULL)
		// transferring all children is vacuous when there are no children
		return;

	while (true) {
		LockFreeXMLNode* privateLastChild = newParent->lastChild;
		firstChildToTransfer->previousSibling = privateLastChild;
		if (cas<LockFreeXMLNode*>(&privateLastChild->nextSibling, NULL, firstChildToTransfer)) {
			// children successfully hooked on
			// if this cas fails, it means that the lastChild pointer is going to have to advance
			// individually through the new nodes. It's fine for correctness but not ideal for speed
			cas<LockFreeXMLNode*>(&newParent->lastChild, privateLastChild, lastChild);
			faa<int>(&newParent->m_childCount, m_childCount);
			break;
		}
		else // privateLastChild->nextSibling != null so newParent->lastChild was lagging
			cas<LockFreeXMLNode*>(&newParent->lastChild, privateLastChild, privateLastChild->nextSibling);
	}
	// Update this
	firstChild = lastChild = NULL;

}

void LockFreeXMLNode::AddChild(LockFreeXMLNode* newChild) {
	assert(newChild->attrStatus != OPEN);
	// Before the node is in the tree, it should be private, so we don't need to be atomic yet
	newChild->nextSibling = NULL;
	newChild->parent = this;
	newChild->m_ancestorCount = m_ancestorCount+1;

	if (firstChild == NULL && cas<LockFreeXMLNode*>(&firstChild, NULL, newChild)) {
		// this is the first node
		lastChild = newChild;
		return;
	}
	if (lastChild == NULL)
		// might fail, but everyone is writing the same value so someone will get it
		// It's still necessary because C++ mem model doesn't guarantee no out-of-thin-air vals
		cas<LockFreeXMLNode*>(&lastChild, NULL, firstChild);

	while (true) {
		LockFreeXMLNode* privateLastChild = lastChild; // other threads may be modifying lastChild
		// anticipate success
		newChild->previousSibling = privateLastChild;

		// privateLastChild->nextSibling == NULL iff privateLastChild is the last one
		if (privateLastChild->nextSibling == NULL
				&& cas<LockFreeXMLNode*>(&(privateLastChild->nextSibling), NULL, newChild)) {
			// Successfully hooked on
			fai(&m_childCount);
			// again, we don't really care if this fails
			cas<LockFreeXMLNode*>(&lastChild, privateLastChild,  newChild);
			return;
		} else
			cas<LockFreeXMLNode*>(&lastChild, privateLastChild, privateLastChild->nextSibling);
	}
}