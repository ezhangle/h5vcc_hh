/*
 * Copyright (C) 2009 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Google Inc. All rights reserved.
 * Copyright (C) 2009 Joseph Pecoraro
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "InspectorDOMAgent.h"

#if ENABLE(INSPECTOR)

#include "AtomicString.h"
#include "CSSComputedStyleDeclaration.h"
#include "CSSMutableStyleDeclaration.h"
#include "CSSRule.h"
#include "CSSRuleList.h"
#include "CSSStyleRule.h"
#include "CSSStyleSelector.h"
#include "CSSStyleSheet.h"
#include "ContainerNode.h"
#include "Cookie.h"
#include "CookieJar.h"
#include "DOMWindow.h"
#include "Document.h"
#include "DocumentType.h"
#include "Event.h"
#include "EventListener.h"
#include "EventNames.h"
#include "EventTarget.h"
#include "Frame.h"
#include "FrameTree.h"
#include "HTMLElement.h"
#include "HTMLFrameOwnerElement.h"
#include "MutationEvent.h"
#include "Node.h"
#include "NodeList.h"
#include "Pasteboard.h"
#include "PlatformString.h"
#include "RemoteInspectorFrontend.h"
#include "RenderStyle.h"
#include "RenderStyleConstants.h"
#include "ScriptEventListener.h"
#include "StyleSheetList.h"
#include "Text.h"

#if ENABLE(XPATH)
#include "XPathResult.h"
#endif

#include "markup.h"

#include <wtf/text/CString.h>
#include <wtf/HashSet.h>
#include <wtf/ListHashSet.h>
#include <wtf/OwnPtr.h>
#include <wtf/Vector.h>

namespace WebCore {

class MatchJob {
public:
    virtual void match(ListHashSet<Node*>& resultCollector) = 0;
    virtual ~MatchJob() { }

protected:
    MatchJob(Document* document, const String& query)
        : m_document(document)
        , m_query(query) { }

    void addNodesToResults(PassRefPtr<NodeList> nodes, ListHashSet<Node*>& resultCollector)
    {
        for (unsigned i = 0; nodes && i < nodes->length(); ++i)
            resultCollector.add(nodes->item(i));
    }

    RefPtr<Document> m_document;
    String m_query;
};

namespace {

class MatchExactIdJob : public WebCore::MatchJob {
public:
    MatchExactIdJob(Document* document, const String& query) : WebCore::MatchJob(document, query) { }
    virtual ~MatchExactIdJob() { }

protected:
    virtual void match(ListHashSet<Node*>& resultCollector)
    {
        if (m_query.isEmpty())
            return;

        Element* element = m_document->getElementById(m_query);
        if (element)
            resultCollector.add(element);
    }
};

class MatchExactClassNamesJob : public WebCore::MatchJob {
public:
    MatchExactClassNamesJob(Document* document, const String& query) : WebCore::MatchJob(document, query) { }
    virtual ~MatchExactClassNamesJob() { }

    virtual void match(ListHashSet<Node*>& resultCollector)
    {
        if (!m_query.isEmpty())
            addNodesToResults(m_document->getElementsByClassName(m_query), resultCollector);
    }
};

class MatchExactTagNamesJob : public WebCore::MatchJob {
public:
    MatchExactTagNamesJob(Document* document, const String& query) : WebCore::MatchJob(document, query) { }
    virtual ~MatchExactTagNamesJob() { }

    virtual void match(ListHashSet<Node*>& resultCollector)
    {
        if (!m_query.isEmpty())
            addNodesToResults(m_document->getElementsByName(m_query), resultCollector);
    }
};

class MatchQuerySelectorAllJob : public WebCore::MatchJob {
public:
    MatchQuerySelectorAllJob(Document* document, const String& query) : WebCore::MatchJob(document, query) { }
    virtual ~MatchQuerySelectorAllJob() { }

    virtual void match(ListHashSet<Node*>& resultCollector)
    {
        if (m_query.isEmpty())
            return;

        ExceptionCode ec = 0;
        RefPtr<NodeList> list = m_document->querySelectorAll(m_query, ec);
        if (!ec)
            addNodesToResults(list, resultCollector);
    }
};

class MatchXPathJob : public WebCore::MatchJob {
public:
    MatchXPathJob(Document* document, const String& query) : WebCore::MatchJob(document, query) { }
    virtual ~MatchXPathJob() { }

    virtual void match(ListHashSet<Node*>& resultCollector)
    {
#if ENABLE(XPATH)
        if (m_query.isEmpty())
            return;

        ExceptionCode ec = 0;
        RefPtr<XPathResult> result = m_document->evaluate(m_query, m_document.get(), 0, XPathResult::ORDERED_NODE_SNAPSHOT_TYPE, 0, ec);
        if (ec || !result)
            return;

        unsigned long size = result->snapshotLength(ec);
        for (unsigned long i = 0; !ec && i < size; ++i) {
            Node* node = result->snapshotItem(i, ec);
            if (!ec)
                resultCollector.add(node);
        }
#endif
    }
};

class MatchPlainTextJob : public MatchXPathJob {
public:
    MatchPlainTextJob(Document* document, const String& query) : MatchXPathJob(document, query)
    {
        m_query = "//text()[contains(., '" + m_query + "')] | //comment()[contains(., '" + m_query + "')]";
    }
    virtual ~MatchPlainTextJob() { }
};

}

InspectorDOMAgent::InspectorDOMAgent(InspectorCSSStore* cssStore, RemoteInspectorFrontend* frontend)
    : EventListener(InspectorDOMAgentType)
    , m_cssStore(cssStore)
    , m_frontend(frontend)
    , m_lastNodeId(1)
    , m_matchJobsTimer(this, &InspectorDOMAgent::onMatchJobsTimer)
{
}

InspectorDOMAgent::~InspectorDOMAgent()
{
    reset();
}

void InspectorDOMAgent::reset()
{
    searchCanceled();
    discardBindings();

    ListHashSet<RefPtr<Document> > copy = m_documents;
    for (ListHashSet<RefPtr<Document> >::iterator it = copy.begin(); it != copy.end(); ++it)
        stopListening((*it).get());

    ASSERT(!m_documents.size());
}

void InspectorDOMAgent::setDocument(Document* doc)
{
    if (doc == mainFrameDocument())
        return;

    reset();

    if (doc) {
        startListening(doc);
        if (doc->documentElement())
            pushDocumentToFrontend();
    } else
        m_frontend->setDocument(InspectorValue::null());
}

void InspectorDOMAgent::releaseDanglingNodes()
{
    deleteAllValues(m_danglingNodeToIdMaps);
    m_danglingNodeToIdMaps.clear();
}

void InspectorDOMAgent::startListening(Document* doc)
{
    if (m_documents.contains(doc))
        return;

    doc->addEventListener(eventNames().DOMContentLoadedEvent, this, false);
    doc->addEventListener(eventNames().loadEvent, this, true);
    m_documents.add(doc);
}

void InspectorDOMAgent::stopListening(Document* doc)
{
    if (!m_documents.contains(doc))
        return;

    doc->removeEventListener(eventNames().DOMContentLoadedEvent, this, false);
    doc->removeEventListener(eventNames().loadEvent, this, true);
    m_documents.remove(doc);
}

void InspectorDOMAgent::handleEvent(ScriptExecutionContext*, Event* event)
{
    AtomicString type = event->type();
    Node* node = event->target()->toNode();

    if (type == eventNames().DOMContentLoadedEvent) {
        // Re-push document once it is loaded.
        discardBindings();
        pushDocumentToFrontend();
    } else if (type == eventNames().loadEvent) {
        long frameOwnerId = m_documentNodeToIdMap.get(node);
        if (!frameOwnerId)
            return;

        if (!m_childrenRequested.contains(frameOwnerId)) {
            // No children are mapped yet -> only notify on changes of hasChildren.
            m_frontend->childNodeCountUpdated(frameOwnerId, innerChildNodeCount(node));
        } else {
            // Re-add frame owner element together with its new children.
            long parentId = m_documentNodeToIdMap.get(innerParentNode(node));
            m_frontend->childNodeRemoved(parentId, frameOwnerId);
            RefPtr<InspectorObject> value = buildObjectForNode(node, 0, &m_documentNodeToIdMap);
            Node* previousSibling = innerPreviousSibling(node);
            long prevId = previousSibling ? m_documentNodeToIdMap.get(previousSibling) : 0;
            m_frontend->childNodeInserted(parentId, prevId, value.release());
            // Invalidate children requested flag for the element.
            m_childrenRequested.remove(m_childrenRequested.find(frameOwnerId));
        }
    }
}

long InspectorDOMAgent::bind(Node* node, NodeToIdMap* nodesMap)
{
    long id = nodesMap->get(node);
    if (id)
        return id;
    id = m_lastNodeId++;
    nodesMap->set(node, id);
    m_idToNode.set(id, node);
    m_idToNodesMap.set(id, nodesMap);
    return id;
}

void InspectorDOMAgent::unbind(Node* node, NodeToIdMap* nodesMap)
{
    if (node->isFrameOwnerElement()) {
        const HTMLFrameOwnerElement* frameOwner = static_cast<const HTMLFrameOwnerElement*>(node);
        stopListening(frameOwner->contentDocument());
        cssStore()->removeDocument(frameOwner->contentDocument());
    }

    long id = nodesMap->get(node);
    if (!id)
        return;
    m_idToNode.remove(id);
    nodesMap->remove(node);
    bool childrenRequested = m_childrenRequested.contains(id);
    if (childrenRequested) {
        // Unbind subtree known to client recursively.
        m_childrenRequested.remove(id);
        Node* child = innerFirstChild(node);
        while (child) {
            unbind(child, nodesMap);
            child = innerNextSibling(child);
        }
    }
}

bool InspectorDOMAgent::pushDocumentToFrontend()
{
    Document* document = mainFrameDocument();
    if (!document)
        return false;
    if (!m_documentNodeToIdMap.contains(document))
        m_frontend->setDocument(buildObjectForNode(document, 2, &m_documentNodeToIdMap));
    return true;
}

void InspectorDOMAgent::pushChildNodesToFrontend(long nodeId)
{
    Node* node = nodeForId(nodeId);
    if (!node || (node->nodeType() != Node::ELEMENT_NODE && node->nodeType() != Node::DOCUMENT_NODE && node->nodeType() != Node::DOCUMENT_FRAGMENT_NODE))
        return;
    if (m_childrenRequested.contains(nodeId))
        return;

    NodeToIdMap* nodeMap = m_idToNodesMap.get(nodeId);
    RefPtr<InspectorArray> children = buildArrayForContainerChildren(node, 1, nodeMap);
    m_childrenRequested.add(nodeId);
    m_frontend->setChildNodes(nodeId, children.release());
}

long InspectorDOMAgent::inspectedNode(unsigned long num)
{
    if (num < m_inspectedNodes.size())
        return m_inspectedNodes[num];
    return 0;
}

void InspectorDOMAgent::discardBindings()
{
    m_documentNodeToIdMap.clear();
    m_idToNode.clear();
    releaseDanglingNodes();
    m_childrenRequested.clear();
    m_inspectedNodes.clear();
}

Node* InspectorDOMAgent::nodeForId(long id)
{
    if (!id)
        return 0;

    HashMap<long, Node*>::iterator it = m_idToNode.find(id);
    if (it != m_idToNode.end())
        return it->second;
    return 0;
}

void InspectorDOMAgent::getChildNodes(long callId, long nodeId)
{
    pushChildNodesToFrontend(nodeId);
    m_frontend->didGetChildNodes(callId);
}

long InspectorDOMAgent::pushNodePathToFrontend(Node* nodeToPush)
{
    ASSERT(nodeToPush);  // Invalid input

    // If we are sending information to the client that is currently being created. Send root node first.
    if (!pushDocumentToFrontend())
        return 0;

    // Return id in case the node is known.
    long result = m_documentNodeToIdMap.get(nodeToPush);
    if (result)
        return result;

    Node* node = nodeToPush;
    Vector<Node*> path;
    NodeToIdMap* danglingMap = 0;
    while (true) {
        Node* parent = innerParentNode(node);
        if (!parent) {
            // Node being pushed is detached -> push subtree root.
            danglingMap = new NodeToIdMap();
            m_danglingNodeToIdMaps.append(danglingMap);
            m_frontend->setDetachedRoot(buildObjectForNode(node, 0, danglingMap));
            break;
        } else {
            path.append(parent);
            if (m_documentNodeToIdMap.get(parent))
                break;
            else
                node = parent;
        }
    }

    NodeToIdMap* map = danglingMap ? danglingMap : &m_documentNodeToIdMap;
    for (int i = path.size() - 1; i >= 0; --i) {
        long nodeId = map->get(path.at(i));
        ASSERT(nodeId);
        pushChildNodesToFrontend(nodeId);
    }
    return map->get(nodeToPush);
}

void InspectorDOMAgent::setAttribute(long callId, long elementId, const String& name, const String& value)
{
    Node* node = nodeForId(elementId);
    if (node && (node->nodeType() == Node::ELEMENT_NODE)) {
        Element* element = static_cast<Element*>(node);
        ExceptionCode ec = 0;
        element->setAttribute(name, value, ec);
        m_frontend->didApplyDomChange(callId, ec == 0);
    } else {
        m_frontend->didApplyDomChange(callId, false);
    }
}

void InspectorDOMAgent::removeAttribute(long callId, long elementId, const String& name)
{
    Node* node = nodeForId(elementId);
    if (node && (node->nodeType() == Node::ELEMENT_NODE)) {
        Element* element = static_cast<Element*>(node);
        ExceptionCode ec = 0;
        element->removeAttribute(name, ec);
        m_frontend->didApplyDomChange(callId, ec == 0);
    } else {
        m_frontend->didApplyDomChange(callId, false);
    }
}

void InspectorDOMAgent::removeNode(long callId, long nodeId)
{
    Node* node = nodeForId(nodeId);
    if (!node) {
        m_frontend->didRemoveNode(callId, 0);
        return;
    }

    Node* parentNode = node->parentNode();
    if (!parentNode) {
        m_frontend->didRemoveNode(callId, 0);
        return;
    }

    ExceptionCode ec = 0;
    parentNode->removeChild(node, ec);
    if (ec) {
        m_frontend->didRemoveNode(callId, 0);
        return;
    }

    m_frontend->didRemoveNode(callId, nodeId);
}

void InspectorDOMAgent::changeTagName(long callId, long nodeId, const String& tagName)
{
    Node* oldNode = nodeForId(nodeId);
    if (!oldNode || !oldNode->isElementNode()) {
        m_frontend->didChangeTagName(callId, 0);
        return;
    }

    bool childrenRequested = m_childrenRequested.contains(nodeId);

    ExceptionCode ec = 0;
    RefPtr<Element> newElem = oldNode->document()->createElement(tagName, ec);
    if (ec) {
        m_frontend->didChangeTagName(callId, 0);
        return;
    }

    // Copy over the original node's attributes.
    Element* oldElem = static_cast<Element*>(oldNode);
    newElem->copyNonAttributeProperties(oldElem);
    if (oldElem->attributes())
        newElem->attributes()->setAttributes(*(oldElem->attributes(true)));

    // Copy over the original node's children.
    Node* child;
    while ((child = oldNode->firstChild()))
        newElem->appendChild(child, ec);

    // Replace the old node with the new node
    Node* parent = oldNode->parentNode();
    parent->insertBefore(newElem, oldNode->nextSibling(), ec);
    parent->removeChild(oldNode, ec);

    if (ec) {
        m_frontend->didChangeTagName(callId, 0);
        return;
    }

    long newId = pushNodePathToFrontend(newElem.get());
    if (childrenRequested)
        pushChildNodesToFrontend(newId);
    m_frontend->didChangeTagName(callId, newId);
}

void InspectorDOMAgent::getOuterHTML(long callId, long nodeId)
{
    Node* node = nodeForId(nodeId);
    if (!node || !node->isHTMLElement()) {
        m_frontend->didGetOuterHTML(callId, "");
        return;
    }

    HTMLElement* htmlElement = static_cast<HTMLElement*>(node);
    m_frontend->didGetOuterHTML(callId, htmlElement->outerHTML());
}

void InspectorDOMAgent::setOuterHTML(long callId, long nodeId, const String& outerHTML)
{
    Node* node = nodeForId(nodeId);
    if (!node || !node->isHTMLElement()) {
        m_frontend->didSetOuterHTML(callId, 0);
        return;
    }

    bool childrenRequested = m_childrenRequested.contains(nodeId);
    Node* previousSibling = node->previousSibling();
    Node* parentNode = node->parentNode();

    HTMLElement* htmlElement = static_cast<HTMLElement*>(node);
    ExceptionCode ec = 0;
    htmlElement->setOuterHTML(outerHTML, ec);
    if (ec)
        m_frontend->didSetOuterHTML(callId, 0);

    Node* newNode = previousSibling ? previousSibling->nextSibling() : parentNode->firstChild();

    long newId = pushNodePathToFrontend(newNode);
    if (childrenRequested)
        pushChildNodesToFrontend(newId);

    m_frontend->didSetOuterHTML(callId, newId);
}

void InspectorDOMAgent::setTextNodeValue(long callId, long nodeId, const String& value)
{
    Node* node = nodeForId(nodeId);
    if (node && (node->nodeType() == Node::TEXT_NODE)) {
        Text* text_node = static_cast<Text*>(node);
        ExceptionCode ec = 0;
        text_node->replaceWholeText(value, ec);
        m_frontend->didApplyDomChange(callId, ec == 0);
    } else {
        m_frontend->didApplyDomChange(callId, false);
    }
}

void InspectorDOMAgent::getEventListenersForNode(long callId, long nodeId)
{
    Node* node = nodeForId(nodeId);
    RefPtr<InspectorArray> listenersArray = InspectorArray::create();
    EventTargetData* d;

    // Quick break if a null node or no listeners at all
    if (!node || !(d = node->eventTargetData())) {
        m_frontend->didGetEventListenersForNode(callId, nodeId, listenersArray.release());
        return;
    }

    // Get the list of event types this Node is concerned with
    Vector<AtomicString> eventTypes;
    const EventListenerMap& listenerMap = d->eventListenerMap;
    EventListenerMap::const_iterator end = listenerMap.end();
    for (EventListenerMap::const_iterator iter = listenerMap.begin(); iter != end; ++iter)
        eventTypes.append(iter->first);

    // Quick break if no useful listeners
    size_t eventTypesLength = eventTypes.size();
    if (eventTypesLength == 0) {
        m_frontend->didGetEventListenersForNode(callId, nodeId, listenersArray.release());
        return;
    }

    // The Node's Event Ancestors (not including self)
    Vector<RefPtr<ContainerNode> > ancestors;
    node->eventAncestors(ancestors);

    // Nodes and their Listeners for the concerned event types (order is top to bottom)
    Vector<EventListenerInfo> eventInformation;
    for (size_t i = ancestors.size(); i; --i) {
        ContainerNode* ancestor = ancestors[i - 1].get();
        for (size_t j = 0; j < eventTypesLength; ++j) {
            AtomicString& type = eventTypes[j];
            if (ancestor->hasEventListeners(type))
                eventInformation.append(EventListenerInfo(static_cast<Node*>(ancestor), type, ancestor->getEventListeners(type)));
        }
    }

    // Insert the Current Node at the end of that list (last in capturing, first in bubbling)
    for (size_t i = 0; i < eventTypesLength; ++i) {
        const AtomicString& type = eventTypes[i];
        eventInformation.append(EventListenerInfo(node, type, node->getEventListeners(type)));
    }

    // Get Capturing Listeners (in this order)
    size_t eventInformationLength = eventInformation.size();
    for (size_t i = 0; i < eventInformationLength; ++i) {
        const EventListenerInfo& info = eventInformation[i];
        const EventListenerVector& vector = info.eventListenerVector;
        for (size_t j = 0; j < vector.size(); ++j) {
            const RegisteredEventListener& listener = vector[j];
            if (listener.useCapture)
                listenersArray->push(buildObjectForEventListener(listener, info.eventType, info.node));
        }
    }

    // Get Bubbling Listeners (reverse order)
    for (size_t i = eventInformationLength; i; --i) {
        const EventListenerInfo& info = eventInformation[i - 1];
        const EventListenerVector& vector = info.eventListenerVector;
        for (size_t j = 0; j < vector.size(); ++j) {
            const RegisteredEventListener& listener = vector[j];
            if (!listener.useCapture)
                listenersArray->push(buildObjectForEventListener(listener, info.eventType, info.node));
        }
    }

    m_frontend->didGetEventListenersForNode(callId, nodeId, listenersArray.release());
}

void InspectorDOMAgent::addInspectedNode(long nodeId)
{
    m_inspectedNodes.prepend(nodeId);
    while (m_inspectedNodes.size() > 5)
        m_inspectedNodes.removeLast();
}

void InspectorDOMAgent::performSearch(const String& whitespaceTrimmedQuery, bool runSynchronously)
{
    // FIXME: Few things are missing here:
    // 1) Search works with node granularity - number of matches within node is not calculated.
    // 2) There is no need to push all search results to the front-end at a time, pushing next / previous result
    //    is sufficient.

    int queryLength = whitespaceTrimmedQuery.length();
    bool startTagFound = !whitespaceTrimmedQuery.find('<');
    bool endTagFound = whitespaceTrimmedQuery.reverseFind('>') + 1 == queryLength;

    String tagNameQuery = whitespaceTrimmedQuery;
    if (startTagFound || endTagFound)
        tagNameQuery = tagNameQuery.substring(startTagFound ? 1 : 0, endTagFound ? queryLength - 1 : queryLength);
    if (!Document::isValidName(tagNameQuery))
        tagNameQuery = "";

    String attributeNameQuery = whitespaceTrimmedQuery;
    if (!Document::isValidName(attributeNameQuery))
        attributeNameQuery = "";

    String escapedQuery = whitespaceTrimmedQuery;
    escapedQuery.replace("'", "\\'");
    String escapedTagNameQuery = tagNameQuery;
    escapedTagNameQuery.replace("'", "\\'");

    // Clear pending jobs.
    searchCanceled();

    // Find all frames, iframes and object elements to search their documents.
    for (Frame* frame = mainFrameDocument()->frame(); frame; frame = frame->tree()->traverseNext()) {
        Document* document = frame->document();
        if (!document)
            continue;

        if (!tagNameQuery.isEmpty() && startTagFound && endTagFound) {
            m_pendingMatchJobs.append(new MatchExactTagNamesJob(document, tagNameQuery));
            m_pendingMatchJobs.append(new MatchPlainTextJob(document, escapedQuery));
            continue;
        }

        if (!tagNameQuery.isEmpty() && startTagFound) {
            m_pendingMatchJobs.append(new MatchXPathJob(document, "//*[starts-with(name(), '" + escapedTagNameQuery + "')]"));
            m_pendingMatchJobs.append(new MatchPlainTextJob(document, escapedQuery));
            continue;
        }

        if (!tagNameQuery.isEmpty() && endTagFound) {
            // FIXME: we should have a matchEndOfTagNames search function if endTagFound is true but not startTagFound.
            // This requires ends-with() support in XPath, WebKit only supports starts-with() and contains().
            m_pendingMatchJobs.append(new MatchXPathJob(document, "//*[contains(name(), '" + escapedTagNameQuery + "')]"));
            m_pendingMatchJobs.append(new MatchPlainTextJob(document, escapedQuery));
            continue;
        }

        bool matchesEveryNode = whitespaceTrimmedQuery == "//*" || whitespaceTrimmedQuery == "*";
        if (matchesEveryNode) {
            // These queries will match every node. Matching everything isn't useful and can be slow for large pages,
            // so limit the search functions list to plain text and attribute matching for these.
            m_pendingMatchJobs.append(new MatchXPathJob(document, "//*[contains(@*, '" + escapedQuery + "')]"));
            m_pendingMatchJobs.append(new MatchPlainTextJob(document, escapedQuery));
            continue;
        }
            
        m_pendingMatchJobs.append(new MatchExactIdJob(document, whitespaceTrimmedQuery));
        m_pendingMatchJobs.append(new MatchExactClassNamesJob(document, whitespaceTrimmedQuery));
        m_pendingMatchJobs.append(new MatchExactTagNamesJob(document, tagNameQuery));
        m_pendingMatchJobs.append(new MatchQuerySelectorAllJob(document, "[" + attributeNameQuery + "]"));
        m_pendingMatchJobs.append(new MatchQuerySelectorAllJob(document, whitespaceTrimmedQuery));
        m_pendingMatchJobs.append(new MatchXPathJob(document, "//*[contains(@*, '" + escapedQuery + "')]"));
        if (!tagNameQuery.isEmpty())
            m_pendingMatchJobs.append(new MatchXPathJob(document, "//*[contains(name(), '" + escapedTagNameQuery + "')]"));
        m_pendingMatchJobs.append(new MatchPlainTextJob(document, escapedQuery));
        m_pendingMatchJobs.append(new MatchXPathJob(document, whitespaceTrimmedQuery));
    }

    if (runSynchronously) {
        // For tests.
        ListHashSet<Node*> resultCollector;
        for (Deque<MatchJob*>::iterator it = m_pendingMatchJobs.begin(); it != m_pendingMatchJobs.end(); ++it)
            (*it)->match(resultCollector);
        reportNodesAsSearchResults(resultCollector);
        searchCanceled();
        return;
    }
    m_matchJobsTimer.startOneShot(0);
}

void InspectorDOMAgent::searchCanceled()
{
    if (m_matchJobsTimer.isActive())
        m_matchJobsTimer.stop();
    deleteAllValues(m_pendingMatchJobs);
    m_pendingMatchJobs.clear();
    m_searchResults.clear();
}

String InspectorDOMAgent::documentURLString(Document* document) const
{
    if (!document || document->url().isNull())
        return "";
    return document->url().string();
}

PassRefPtr<InspectorObject> InspectorDOMAgent::buildObjectForNode(Node* node, int depth, NodeToIdMap* nodesMap)
{
    RefPtr<InspectorObject> value = InspectorObject::create();

    long id = bind(node, nodesMap);
    String nodeName;
    String localName;
    String nodeValue;

    switch (node->nodeType()) {
        case Node::TEXT_NODE:
        case Node::COMMENT_NODE:
            nodeValue = node->nodeValue();
            break;
        case Node::ATTRIBUTE_NODE:
            localName = node->localName();
            break;
        case Node::DOCUMENT_FRAGMENT_NODE:
            break;
        case Node::DOCUMENT_NODE:
        case Node::ELEMENT_NODE:
        default:
            nodeName = node->nodeName();
            localName = node->localName();
            break;
    }

    value->setNumber("id", id);
    value->setNumber("nodeType", node->nodeType());
    value->setString("nodeName", nodeName);
    value->setString("localName", localName);
    value->setString("nodeValue", nodeValue);

    if (node->nodeType() == Node::ELEMENT_NODE || node->nodeType() == Node::DOCUMENT_NODE || node->nodeType() == Node::DOCUMENT_FRAGMENT_NODE) {
        int nodeCount = innerChildNodeCount(node);
        value->setNumber("childNodeCount", nodeCount);
        RefPtr<InspectorArray> children = buildArrayForContainerChildren(node, depth, nodesMap);
        if (children->length() > 0)
            value->set("children", children.release());

        if (node->nodeType() == Node::ELEMENT_NODE) {
            Element* element = static_cast<Element*>(node);
            value->set("attributes", buildArrayForElementAttributes(element));
            if (node->isFrameOwnerElement()) {
                HTMLFrameOwnerElement* frameOwner = static_cast<HTMLFrameOwnerElement*>(node);
                value->setString("documentURL", documentURLString(frameOwner->contentDocument()));
            }
        } else if (node->nodeType() == Node::DOCUMENT_NODE) {
            Document* document = static_cast<Document*>(node);
            value->setString("documentURL", documentURLString(document));
        }
    } else if (node->nodeType() == Node::DOCUMENT_TYPE_NODE) {
        DocumentType* docType = static_cast<DocumentType*>(node);
        value->setString("publicId", docType->publicId());
        value->setString("systemId", docType->systemId());
        value->setString("internalSubset", docType->internalSubset());
    }
    return value.release();
}

PassRefPtr<InspectorArray> InspectorDOMAgent::buildArrayForElementAttributes(Element* element)
{
    RefPtr<InspectorArray> attributesValue = InspectorArray::create();
    // Go through all attributes and serialize them.
    const NamedNodeMap* attrMap = element->attributes(true);
    if (!attrMap)
        return attributesValue.release();
    unsigned numAttrs = attrMap->length();
    for (unsigned i = 0; i < numAttrs; ++i) {
        // Add attribute pair
        const Attribute *attribute = attrMap->attributeItem(i);
        attributesValue->pushString(attribute->name().toString());
        attributesValue->pushString(attribute->value());
    }
    return attributesValue.release();
}

PassRefPtr<InspectorArray> InspectorDOMAgent::buildArrayForContainerChildren(Node* container, int depth, NodeToIdMap* nodesMap)
{
    RefPtr<InspectorArray> children = InspectorArray::create();
    if (depth == 0) {
        // Special case the_only text child.
        if (innerChildNodeCount(container) == 1) {
            Node *child = innerFirstChild(container);
            if (child->nodeType() == Node::TEXT_NODE)
                children->push(buildObjectForNode(child, 0, nodesMap));
        }
        return children.release();
    } else if (depth > 0) {
        depth--;
    }

    for (Node *child = innerFirstChild(container); child; child = innerNextSibling(child))
        children->push(buildObjectForNode(child, depth, nodesMap));
    return children.release();
}

PassRefPtr<InspectorObject> InspectorDOMAgent::buildObjectForEventListener(const RegisteredEventListener& registeredEventListener, const AtomicString& eventType, Node* node)
{
    RefPtr<EventListener> eventListener = registeredEventListener.listener;
    RefPtr<InspectorObject> value = InspectorObject::create();
    value->setString("type", eventType);
    value->setBool("useCapture", registeredEventListener.useCapture);
    value->setBool("isAttribute", eventListener->isAttribute());
    value->setNumber("nodeId", pushNodePathToFrontend(node));
    value->setString("listenerBody", eventListenerHandlerBody(node->document(), eventListener.get()));
    String sourceName;
    int lineNumber;
    if (eventListenerHandlerLocation(node->document(), eventListener.get(), sourceName, lineNumber)) {
        value->setString("sourceName", sourceName);
        value->setNumber("lineNumber", lineNumber);
    }
    return value.release();
}

Node* InspectorDOMAgent::innerFirstChild(Node* node)
{
    if (node->isFrameOwnerElement()) {
        HTMLFrameOwnerElement* frameOwner = static_cast<HTMLFrameOwnerElement*>(node);
        Document* doc = frameOwner->contentDocument();
        if (doc) {
            startListening(doc);
            return doc->firstChild();
        }
    }
    node = node->firstChild();
    while (isWhitespace(node))
        node = node->nextSibling();
    return node;
}

Node* InspectorDOMAgent::innerNextSibling(Node* node)
{
    do {
        node = node->nextSibling();
    } while (isWhitespace(node));
    return node;
}

Node* InspectorDOMAgent::innerPreviousSibling(Node* node)
{
    do {
        node = node->previousSibling();
    } while (isWhitespace(node));
    return node;
}

unsigned InspectorDOMAgent::innerChildNodeCount(Node* node)
{
    unsigned count = 0;
    Node* child = innerFirstChild(node);
    while (child) {
        count++;
        child = innerNextSibling(child);
    }
    return count;
}

Node* InspectorDOMAgent::innerParentNode(Node* node)
{
    Node* parent = node->parentNode();
    if (parent && parent->nodeType() == Node::DOCUMENT_NODE)
        return static_cast<Document*>(parent)->ownerElement();
    return parent;
}

bool InspectorDOMAgent::isWhitespace(Node* node)
{
    //TODO: pull ignoreWhitespace setting from the frontend and use here.
    return node && node->nodeType() == Node::TEXT_NODE && node->nodeValue().stripWhiteSpace().length() == 0;
}

Document* InspectorDOMAgent::mainFrameDocument() const
{
    ListHashSet<RefPtr<Document> >::const_iterator it = m_documents.begin();
    if (it != m_documents.end())
        return it->get();
    return 0;
}

bool InspectorDOMAgent::operator==(const EventListener& listener)
{
    if (const InspectorDOMAgent* inspectorDOMAgentListener = InspectorDOMAgent::cast(&listener))
        return mainFrameDocument() == inspectorDOMAgentListener->mainFrameDocument();
    return false;
}

void InspectorDOMAgent::didInsertDOMNode(Node* node)
{
    if (isWhitespace(node))
        return;

    // We could be attaching existing subtree. Forget the bindings.
    unbind(node, &m_documentNodeToIdMap);

    Node* parent = node->parentNode();
    long parentId = m_documentNodeToIdMap.get(parent);
    // Return if parent is not mapped yet.
    if (!parentId)
        return;

    if (!m_childrenRequested.contains(parentId)) {
        // No children are mapped yet -> only notify on changes of hasChildren.
        m_frontend->childNodeCountUpdated(parentId, innerChildNodeCount(parent));
    } else {
        // Children have been requested -> return value of a new child.
        Node* prevSibling = innerPreviousSibling(node);
        long prevId = prevSibling ? m_documentNodeToIdMap.get(prevSibling) : 0;
        RefPtr<InspectorObject> value = buildObjectForNode(node, 0, &m_documentNodeToIdMap);
        m_frontend->childNodeInserted(parentId, prevId, value.release());
    }
}

void InspectorDOMAgent::didRemoveDOMNode(Node* node)
{
    if (isWhitespace(node))
        return;

    Node* parent = node->parentNode();
    long parentId = m_documentNodeToIdMap.get(parent);
    // If parent is not mapped yet -> ignore the event.
    if (!parentId)
        return;

    if (!m_childrenRequested.contains(parentId)) {
        // No children are mapped yet -> only notify on changes of hasChildren.
        if (innerChildNodeCount(parent) == 1)
            m_frontend->childNodeCountUpdated(parentId, 0);
    } else
        m_frontend->childNodeRemoved(parentId, m_documentNodeToIdMap.get(node));
    unbind(node, &m_documentNodeToIdMap);
}

void InspectorDOMAgent::didModifyDOMAttr(Element* element)
{
    long id = m_documentNodeToIdMap.get(element);
    // If node is not mapped yet -> ignore the event.
    if (!id)
        return;

    m_frontend->attributesUpdated(id, buildArrayForElementAttributes(element));
}

void InspectorDOMAgent::getStyles(long callId, long nodeId, bool authorOnly)
{
    Node* node = nodeForId(nodeId);
    if (!node || node->nodeType() != Node::ELEMENT_NODE) {
        m_frontend->didGetStyles(callId, InspectorValue::null());
        return;
    }

    DOMWindow* defaultView = node->ownerDocument()->defaultView();
    if (!defaultView) {
        m_frontend->didGetStyles(callId, InspectorValue::null());
        return;
    }

    Element* element = static_cast<Element*>(node);
    RefPtr<CSSComputedStyleDeclaration> computedStyleInfo = computedStyle(node, true); // Support the viewing of :visited information in computed style.

    RefPtr<InspectorObject> result = InspectorObject::create();
    if (element->style())
        result->set("inlineStyle", buildObjectForStyle(element->style(), true));
    result->set("computedStyle", buildObjectForStyle(computedStyleInfo.get(), false));

    CSSStyleSelector* selector = element->ownerDocument()->styleSelector();
    RefPtr<CSSRuleList> matchedRules = selector->styleRulesForElement(element, authorOnly);
    result->set("matchedCSSRules", buildArrayForCSSRules(node->ownerDocument(), matchedRules.get()));

    result->set("styleAttributes", buildObjectForAttributeStyles(element));
    result->set("pseudoElements", buildArrayForPseudoElements(element, authorOnly));

    RefPtr<InspectorObject> currentStyle = result;
    Element* parentElement = element->parentElement();
    while (parentElement) {
        RefPtr<InspectorObject> parentStyle = InspectorObject::create();
        currentStyle->set("parent", parentStyle);
        if (parentElement->style() && parentElement->style()->length())
            parentStyle->set("inlineStyle", buildObjectForStyle(parentElement->style(), true));

        CSSStyleSelector* parentSelector = parentElement->ownerDocument()->styleSelector();
        RefPtr<CSSRuleList> parentMatchedRules = parentSelector->styleRulesForElement(parentElement, authorOnly);
        parentStyle->set("matchedCSSRules", buildArrayForCSSRules(parentElement->ownerDocument(), parentMatchedRules.get()));

        parentElement = parentElement->parentElement();
        currentStyle = parentStyle;
    }
    m_frontend->didGetStyles(callId, result.release());
}

void InspectorDOMAgent::getAllStyles(long callId)
{
    RefPtr<InspectorArray> result = InspectorArray::create();
    for (ListHashSet<RefPtr<Document> >::iterator it = m_documents.begin(); it != m_documents.end(); ++it) {
        StyleSheetList* list = (*it)->styleSheets();
        for (unsigned i = 0; i < list->length(); ++i) {
            StyleSheet* styleSheet = list->item(i);
            if (styleSheet->isCSSStyleSheet())
                result->push(buildObjectForStyleSheet((*it).get(), static_cast<CSSStyleSheet*>(styleSheet)));
        }
    }
    m_frontend->didGetAllStyles(callId, result.release());
}

void InspectorDOMAgent::getStyleSheet(long callId, long styleSheetId)
{
    CSSStyleSheet* styleSheet = cssStore()->styleSheetForId(styleSheetId);
    if (styleSheet && styleSheet->doc())
        m_frontend->didGetStyleSheet(callId, buildObjectForStyleSheet(styleSheet->doc(), styleSheet));
    else
        m_frontend->didGetStyleSheet(callId, InspectorObject::create());
}

void InspectorDOMAgent::getRuleRangesForStyleSheetId(long callId, long styleSheetId)
{
    CSSStyleSheet* styleSheet = cssStore()->styleSheetForId(styleSheetId);
    if (styleSheet && styleSheet->doc()) {
        HashMap<long, SourceRange> ruleRanges = cssStore()->getRuleRangesForStyleSheet(styleSheet);
        if (!ruleRanges.size()) {
            m_frontend->didGetStyleSheet(callId, InspectorObject::create());
            return;
        }
        RefPtr<InspectorObject> result = InspectorObject::create();
        for (HashMap<long, SourceRange>::iterator it = ruleRanges.begin(); it != ruleRanges.end(); ++it) {
            if (it->second.second) {
                RefPtr<InspectorObject> ruleRange = InspectorObject::create();
                result->set(String::number(it->first).utf8().data(), ruleRange);
                RefPtr<InspectorObject> bodyRange = InspectorObject::create();
                ruleRange->set("bodyRange", bodyRange);
                bodyRange->setNumber("start", it->second.first);
                bodyRange->setNumber("end", it->second.second);
            }
        }
        m_frontend->didGetStyleSheet(callId, result);
    } else
        m_frontend->didGetStyleSheet(callId, InspectorValue::null());
}

void InspectorDOMAgent::getInlineStyle(long callId, long nodeId)
{
    Node* node = nodeForId(nodeId);
    if (!node || node->nodeType() != Node::ELEMENT_NODE) {
        m_frontend->didGetInlineStyle(callId, InspectorValue::null());
        return;
    }
    Element* element = static_cast<Element*>(node);
    m_frontend->didGetInlineStyle(callId, buildObjectForStyle(element->style(), true));
}

void InspectorDOMAgent::getComputedStyle(long callId, long nodeId)
{
    Node* node = nodeForId(nodeId);
    if (!node || node->nodeType() != Node::ELEMENT_NODE) {
        m_frontend->didGetComputedStyle(callId, InspectorValue::null());
        return;
    }

    DOMWindow* defaultView = node->ownerDocument()->defaultView();
    if (!defaultView) {
        m_frontend->didGetComputedStyle(callId, InspectorValue::null());
        return;
    }

    Element* element = static_cast<Element*>(node);
    RefPtr<CSSStyleDeclaration> computedStyle = defaultView->getComputedStyle(element, "");
    m_frontend->didGetComputedStyle(callId, buildObjectForStyle(computedStyle.get(), false));
}

PassRefPtr<InspectorObject> InspectorDOMAgent::buildObjectForAttributeStyles(Element* element)
{
    RefPtr<InspectorObject> styleAttributes = InspectorObject::create();
    NamedNodeMap* attributes = element->attributes();
    for (unsigned i = 0; attributes && i < attributes->length(); ++i) {
        Attribute* attribute = attributes->attributeItem(i);
        if (attribute->style()) {
            String attributeName = attribute->localName();
            styleAttributes->set(attributeName.utf8().data(), buildObjectForStyle(attribute->style(), true));
        }
    }
    return styleAttributes;
}

PassRefPtr<InspectorArray> InspectorDOMAgent::buildArrayForCSSRules(Document* ownerDocument, CSSRuleList* matchedRules)
{
    RefPtr<InspectorArray> matchedCSSRules = InspectorArray::create();
    for (unsigned i = 0; matchedRules && i < matchedRules->length(); ++i) {
        CSSRule* rule = matchedRules->item(i);
        if (rule->type() == CSSRule::STYLE_RULE)
            matchedCSSRules->push(buildObjectForRule(ownerDocument, static_cast<CSSStyleRule*>(rule)));
    }
    return matchedCSSRules.release();
}

PassRefPtr<InspectorArray> InspectorDOMAgent::buildArrayForPseudoElements(Element* element, bool authorOnly)
{
    RefPtr<InspectorArray> result = InspectorArray::create();
    CSSStyleSelector* selector = element->ownerDocument()->styleSelector();
    RefPtr<RenderStyle> renderStyle = element->styleForRenderer();

    for (PseudoId pseudoId = FIRST_PUBLIC_PSEUDOID; pseudoId < AFTER_LAST_INTERNAL_PSEUDOID; pseudoId = static_cast<PseudoId>(pseudoId + 1)) {
        RefPtr<CSSRuleList> matchedRules = selector->pseudoStyleRulesForElement(element, pseudoId, authorOnly);
        if (matchedRules && matchedRules->length()) {
            RefPtr<InspectorObject> pseudoStyles = InspectorObject::create();
            pseudoStyles->setNumber("pseudoId", static_cast<int>(pseudoId));
            pseudoStyles->set("rules", buildArrayForCSSRules(element->ownerDocument(), matchedRules.get()));
            result->push(pseudoStyles.release());
        }
    }
    return result.release();
}

void InspectorDOMAgent::applyStyleText(long callId, long styleId, const String& styleText, const String& propertyName)
{
    CSSStyleDeclaration* style = cssStore()->styleForId(styleId);
    if (!style) {
        m_frontend->didApplyStyleText(callId, false, InspectorValue::null(), InspectorArray::create());
        return;
    }

    // Remove disabled property entry for property with given name.
    DisabledStyleDeclaration* disabledStyle = cssStore()->disabledStyleForId(styleId, false);
    if (disabledStyle)
        disabledStyle->remove(propertyName);

    int styleTextLength = styleText.length();

    RefPtr<CSSMutableStyleDeclaration> tempMutableStyle = CSSMutableStyleDeclaration::create();
    tempMutableStyle->parseDeclaration(styleText);
    CSSStyleDeclaration* tempStyle = static_cast<CSSStyleDeclaration*>(tempMutableStyle.get());

    if (tempStyle->length() || !styleTextLength) {
        ExceptionCode ec = 0;
        // The input was parsable or the user deleted everything, so remove the
        // original property from the real style declaration. If this represents
        // a shorthand remove all the longhand properties.
        if (style->getPropertyShorthand(propertyName).isEmpty()) {
            Vector<String> longhandProps = longhandProperties(style, propertyName);
            for (unsigned i = 0; !ec && i < longhandProps.size(); ++i)
                style->removeProperty(longhandProps[i], ec);
        }
        // Explicitly delete properties with no shorthands as well as shorthands themselves.
        if (!ec)
            style->removeProperty(propertyName, ec);

        if (ec) {
            m_frontend->didApplyStyleText(callId, false, InspectorValue::null(), InspectorArray::create());
            return;
        }
    }

    // Notify caller that the property was successfully deleted.
    if (!styleTextLength) {
        RefPtr<InspectorArray> changedProperties = InspectorArray::create();
        changedProperties->pushString(propertyName);
        m_frontend->didApplyStyleText(callId, true, InspectorValue::null(), changedProperties.release());
        return;
    }

    if (!tempStyle->length()) {
        m_frontend->didApplyStyleText(callId, false, InspectorValue::null(), InspectorArray::create());
        return;
    }

    // Iterate of the properties on the test element's style declaration and
    // add them to the real style declaration. We take care to move shorthands.
    HashSet<String> foundShorthands;
    Vector<String> changedProperties;

    for (unsigned i = 0; i < tempStyle->length(); ++i) {
        String name = tempStyle->item(i);
        String shorthand = tempStyle->getPropertyShorthand(name);

        if (!shorthand.isEmpty() && foundShorthands.contains(shorthand))
            continue;

        String value;
        String priority;
        if (!shorthand.isEmpty()) {
            value = shorthandValue(tempStyle, shorthand);
            priority = shorthandPriority(tempStyle, shorthand);
            foundShorthands.add(shorthand);
            name = shorthand;
        } else {
            value = tempStyle->getPropertyValue(name);
            priority = tempStyle->getPropertyPriority(name);
        }

        // Set the property on the real style declaration.
        ExceptionCode ec = 0;
        style->setProperty(name, value, priority, ec);
        // Remove disabled property entry for property with this name.
        if (disabledStyle)
            disabledStyle->remove(name);
        changedProperties.append(name);
    }
    m_frontend->didApplyStyleText(callId, true, buildObjectForStyle(style, true), toArray(changedProperties));
}

void InspectorDOMAgent::setStyleText(long callId, long styleId, const String& cssText)
{
    CSSStyleDeclaration* style = cssStore()->styleForId(styleId);
    if (!style) {
        m_frontend->didSetStyleText(callId, false);
        return;
    }
    ExceptionCode ec = 0;
    style->setCssText(cssText, ec);
    m_frontend->didSetStyleText(callId, !ec);
}

void InspectorDOMAgent::setStyleProperty(long callId, long styleId, const String& name, const String& value)
{
    CSSStyleDeclaration* style = cssStore()->styleForId(styleId);
    if (!style) {
        m_frontend->didSetStyleProperty(callId, false);
        return;
    }

    ExceptionCode ec = 0;
    style->setProperty(name, value, ec);
    m_frontend->didSetStyleProperty(callId, !ec);
}

void InspectorDOMAgent::toggleStyleEnabled(long callId, long styleId, const String& propertyName, bool disabled)
{
    CSSStyleDeclaration* style = cssStore()->styleForId(styleId);
    if (!style) {
        m_frontend->didToggleStyleEnabled(callId, InspectorValue::null());
        return;
    }

    DisabledStyleDeclaration* disabledStyle = cssStore()->disabledStyleForId(styleId, true);

    // TODO: make sure this works with shorthands right.
    ExceptionCode ec = 0;
    if (disabled) {
        disabledStyle->set(propertyName, std::make_pair(style->getPropertyValue(propertyName), style->getPropertyPriority(propertyName)));
        if (!ec)
            style->removeProperty(propertyName, ec);
    } else if (disabledStyle->contains(propertyName)) {
        PropertyValueAndPriority valueAndPriority = disabledStyle->get(propertyName);
        style->setProperty(propertyName, valueAndPriority.first, valueAndPriority.second, ec);
        if (!ec)
            disabledStyle->remove(propertyName);
    }
    if (ec) {
        m_frontend->didToggleStyleEnabled(callId, InspectorValue::null());
        return;
    }
    m_frontend->didToggleStyleEnabled(callId, buildObjectForStyle(style, true));
}

void InspectorDOMAgent::setRuleSelector(long callId, long ruleId, const String& selector, long selectedNodeId)
{
    CSSStyleRule* rule = cssStore()->ruleForId(ruleId);
    if (!rule) {
        m_frontend->didSetRuleSelector(callId, InspectorValue::null(), false);
        return;
    }

    Node* node = nodeForId(selectedNodeId);

    CSSStyleSheet* styleSheet = rule->parentStyleSheet();
    ExceptionCode ec = 0;
    styleSheet->addRule(selector, rule->style()->cssText(), ec);
    if (ec) {
        m_frontend->didSetRuleSelector(callId, InspectorValue::null(), false);
        return;
    }

    CSSStyleRule* newRule = static_cast<CSSStyleRule*>(styleSheet->item(styleSheet->length() - 1));
    for (unsigned i = 0; i < styleSheet->length(); ++i) {
        if (styleSheet->item(i) == rule) {
            styleSheet->deleteRule(i, ec);
            break;
        }
    }

    if (ec) {
        m_frontend->didSetRuleSelector(callId, InspectorValue::null(), false);
        return;
    }

    m_frontend->didSetRuleSelector(callId, buildObjectForRule(node->ownerDocument(), newRule), ruleAffectsNode(newRule, node));
}

void InspectorDOMAgent::addRule(long callId, const String& selector, long selectedNodeId)
{
    Node* node = nodeForId(selectedNodeId);
    if (!node) {
        m_frontend->didAddRule(callId, InspectorValue::null(), false);
        return;
    }

    CSSStyleSheet* styleSheet = cssStore()->inspectorStyleSheet(node->ownerDocument(), true, callId);
    if (!styleSheet)
        return; // could not add a stylesheet to the ownerDocument

    ExceptionCode ec = 0;
    styleSheet->addRule(selector, "", ec);
    if (ec) {
        m_frontend->didAddRule(callId, InspectorValue::null(), false);
        return;
    }

    CSSStyleRule* newRule = static_cast<CSSStyleRule*>(styleSheet->item(styleSheet->length() - 1));
    m_frontend->didAddRule(callId, buildObjectForRule(node->ownerDocument(), newRule), ruleAffectsNode(newRule, node));
}

PassRefPtr<InspectorObject> InspectorDOMAgent::buildObjectForStyle(CSSStyleDeclaration* style, bool bind)
{
    RefPtr<InspectorObject> result = InspectorObject::create();
    if (bind) {
        long styleId = cssStore()->bindStyle(style);
        result->setNumber("id", styleId);
        CSSStyleSheet* parentStyleSheet = getParentStyleSheet(style);
        if (parentStyleSheet)
            result->setNumber("parentStyleSheetId", cssStore()->bindStyleSheet(parentStyleSheet));

        DisabledStyleDeclaration* disabledStyle = cssStore()->disabledStyleForId(styleId, false);
        if (disabledStyle)
            result->set("disabled", buildArrayForDisabledStyleProperties(disabledStyle));
    }
    result->setString("width", style->getPropertyValue("width"));
    result->setString("height", style->getPropertyValue("height"));
    populateObjectWithStyleProperties(style, result.get());
    return result.release();
}

void InspectorDOMAgent::populateObjectWithStyleProperties(CSSStyleDeclaration* style, InspectorObject* result)
{
    RefPtr<InspectorArray> properties = InspectorArray::create();
    RefPtr<InspectorObject> shorthandValues = InspectorObject::create();

    HashSet<String> foundShorthands;
    for (unsigned i = 0; i < style->length(); ++i) {
        RefPtr<InspectorObject> property = InspectorObject::create();
        String name = style->item(i);
        property->setString("name", name);
        property->setString("priority", style->getPropertyPriority(name));
        property->setBool("implicit", style->isPropertyImplicit(name));
        String shorthand = style->getPropertyShorthand(name);
        property->setString("shorthand", shorthand);
        if (!shorthand.isEmpty() && !foundShorthands.contains(shorthand)) {
            foundShorthands.add(shorthand);
            shorthandValues->setString(shorthand, shorthandValue(style, shorthand));
        }
        property->setString("value", style->getPropertyValue(name));
        properties->push(property.release());
    }
    result->set("properties", properties);
    result->set("shorthandValues", shorthandValues);
}

PassRefPtr<InspectorArray> InspectorDOMAgent::buildArrayForDisabledStyleProperties(DisabledStyleDeclaration* declaration)
{
    RefPtr<InspectorArray> properties = InspectorArray::create();
    for (DisabledStyleDeclaration::iterator it = declaration->begin(); it != declaration->end(); ++it) {
        RefPtr<InspectorObject> property = InspectorObject::create();
        property->setString("name", it->first);
        property->setString("value", it->second.first);
        property->setString("priority", it->second.second);
        properties->push(property.release());
    }
    return properties.release();
}

PassRefPtr<InspectorObject> InspectorDOMAgent::buildObjectForStyleSheet(Document* ownerDocument, CSSStyleSheet* styleSheet)
{
    RefPtr<InspectorObject> result = InspectorObject::create();
    long id = cssStore()->bindStyleSheet(styleSheet);
    result->setNumber("id", id);
    result->setBool("disabled", styleSheet->disabled());
    result->setString("href", styleSheet->href());
    result->setString("title", styleSheet->title());
    result->setNumber("documentElementId", m_documentNodeToIdMap.get(styleSheet->doc()));
    RefPtr<InspectorArray> cssRules = InspectorArray::create();
    PassRefPtr<CSSRuleList> cssRuleList = CSSRuleList::create(styleSheet, true);
    if (cssRuleList) {
        for (unsigned i = 0; i < cssRuleList->length(); ++i) {
            CSSRule* rule = cssRuleList->item(i);
            if (rule->isStyleRule())
                cssRules->push(buildObjectForRule(ownerDocument, static_cast<CSSStyleRule*>(rule)));
        }
    }
    result->set("cssRules", cssRules.release());
    return result.release();
}

PassRefPtr<InspectorObject> InspectorDOMAgent::buildObjectForRule(Document* ownerDocument, CSSStyleRule* rule)
{
    CSSStyleSheet* parentStyleSheet = rule->parentStyleSheet();

    RefPtr<InspectorObject> result = InspectorObject::create();
    result->setString("selectorText", rule->selectorText());
    result->setString("cssText", rule->cssText());
    result->setNumber("sourceLine", rule->sourceLine());
    result->setString("documentURL", documentURLString(ownerDocument));
    if (parentStyleSheet) {
        RefPtr<InspectorObject> parentStyleSheetValue = InspectorObject::create();
        parentStyleSheetValue->setString("href", parentStyleSheet->href());
        parentStyleSheetValue->setNumber("id", cssStore()->bindStyleSheet(parentStyleSheet));
        result->set("parentStyleSheet", parentStyleSheetValue.release());
    }
    bool isUserAgent = parentStyleSheet && !parentStyleSheet->ownerNode() && parentStyleSheet->href().isEmpty();
    bool isUser = parentStyleSheet && parentStyleSheet->ownerNode() && parentStyleSheet->ownerNode()->nodeName() == "#document";
    result->setBool("isUserAgent", isUserAgent);
    result->setBool("isUser", isUser);
    result->setBool("isViaInspector", rule->parentStyleSheet() == cssStore()->inspectorStyleSheet(ownerDocument, false, -1));

    // Bind editable scripts only.
    bool bind = !isUserAgent && !isUser;
    result->set("style", buildObjectForStyle(rule->style(), bind));

    if (bind)
        result->setNumber("id", cssStore()->bindRule(rule));
    return result.release();
}

Vector<String> InspectorDOMAgent::longhandProperties(CSSStyleDeclaration* style, const String& shorthandProperty)
{
    Vector<String> properties;
    HashSet<String> foundProperties;

    for (unsigned i = 0; i < style->length(); ++i) {
        String individualProperty = style->item(i);
        if (foundProperties.contains(individualProperty) || style->getPropertyShorthand(individualProperty) != shorthandProperty)
            continue;
        foundProperties.add(individualProperty);
        properties.append(individualProperty);
    }

    return properties;
}

String InspectorDOMAgent::shorthandValue(CSSStyleDeclaration* style, const String& shorthandProperty)
{
    String value = style->getPropertyValue(shorthandProperty);
    if (value.isEmpty()) {
        // Some shorthands (like border) return a null value, so compute a shorthand value.
        // FIXME: remove this when http://bugs.webkit.org/show_bug.cgi?id=15823 is fixed.
        for (unsigned i = 0; i < style->length(); ++i) {
            String individualProperty = style->item(i);
            if (style->getPropertyShorthand(individualProperty) != shorthandProperty)
                continue;
            if (style->isPropertyImplicit(individualProperty))
                continue;
            String individualValue = style->getPropertyValue(individualProperty);
            if (individualValue == "initial")
                continue;
            if (value.length())
                value.append(" ");
            value.append(individualValue);
        }
    }
    return value;
}

String InspectorDOMAgent::shorthandPriority(CSSStyleDeclaration* style, const String& shorthandProperty)
{
    String priority = style->getPropertyPriority(shorthandProperty);
    if (priority.isEmpty()) {
        for (unsigned i = 0; i < style->length(); ++i) {
            String individualProperty = style->item(i);
            if (style->getPropertyShorthand(individualProperty) != shorthandProperty)
                continue;
            priority = style->getPropertyPriority(individualProperty);
            break;
        }
    }
    return priority;
}

bool InspectorDOMAgent::ruleAffectsNode(CSSStyleRule* rule, Node* node)
{
    if (!node)
        return false;
    ExceptionCode ec = 0;
    RefPtr<NodeList> nodes = node->ownerDocument()->querySelectorAll(rule->selectorText(), ec);
    if (ec)
        return false;
    for (unsigned i = 0; i < nodes->length(); ++i) {
        if (nodes->item(i) == node)
            return true;
    }
    return false;
}

Node* InspectorDOMAgent::nodeForPath(const String& path)
{
    // The path is of form "1,HTML,2,BODY,1,DIV"
    Node* node = mainFrameDocument();
    if (!node)
        return 0;

    Vector<String> pathTokens;
    path.split(",", false, pathTokens);
    if (!pathTokens.size())
        return 0;
    for (size_t i = 0; i < pathTokens.size() - 1; i += 2) {
        bool success = true;
        unsigned childNumber = pathTokens[i].toUInt(&success);
        if (!success)
            return 0;
        if (childNumber >= innerChildNodeCount(node))
            return 0;

        Node* child = innerFirstChild(node);
        String childName = pathTokens[i + 1];
        for (size_t j = 0; child && j < childNumber; ++j)
            child = innerNextSibling(child);

        if (!child || child->nodeName() != childName)
            return 0;
        node = child;
    }
    return node;
}

PassRefPtr<InspectorArray> InspectorDOMAgent::toArray(const Vector<String>& data)
{
    RefPtr<InspectorArray> result = InspectorArray::create();
    for (unsigned i = 0; i < data.size(); ++i)
        result->pushString(data[i]);
    return result.release();
}

CSSStyleSheet* InspectorDOMAgent::getParentStyleSheet(CSSStyleDeclaration* style)
{
    CSSStyleSheet* parentStyleSheet = style->parentRule() ? style->parentRule()->parentStyleSheet() : 0;
    if (!parentStyleSheet) {
        StyleBase* parent = style->parent();
        if (parent && parent->isCSSStyleSheet()) {
            parentStyleSheet = static_cast<CSSStyleSheet*>(parent);
            if (!parentStyleSheet->length())
                return 0;
        }
    }
    return parentStyleSheet;
}

void InspectorDOMAgent::onMatchJobsTimer(Timer<InspectorDOMAgent>*)
{
    if (!m_pendingMatchJobs.size()) {
        searchCanceled();
        return;
    }

    ListHashSet<Node*> resultCollector;
    MatchJob* job = m_pendingMatchJobs.takeFirst();
    job->match(resultCollector);
    delete job;

    reportNodesAsSearchResults(resultCollector);

    m_matchJobsTimer.startOneShot(0.025);
}

void InspectorDOMAgent::reportNodesAsSearchResults(ListHashSet<Node*>& resultCollector)
{
    RefPtr<InspectorArray> nodeIds = InspectorArray::create();
    for (ListHashSet<Node*>::iterator it = resultCollector.begin(); it != resultCollector.end(); ++it) {
        if (m_searchResults.contains(*it))
            continue;
        m_searchResults.add(*it);
        nodeIds->pushNumber(static_cast<long long>(pushNodePathToFrontend(*it)));
    }
    m_frontend->addNodesToSearchResult(nodeIds.release());
}

void InspectorDOMAgent::copyNode(long nodeId)
{
    Node* node = nodeForId(nodeId);
    if (!node)
        return;
    String markup = createMarkup(node);
    Pasteboard::generalPasteboard()->writePlainText(markup);
}

void InspectorDOMAgent::pushNodeByPathToFrontend(long callId, const String& path)
{
    if (!m_frontend)
        return;

    long id = 0;
    Node* node = nodeForPath(path);
    if (node)
        id = pushNodePathToFrontend(node);

    m_frontend->didPushNodeByPathToFrontend(callId, id);
}

} // namespace WebCore

#endif // ENABLE(INSPECTOR)
