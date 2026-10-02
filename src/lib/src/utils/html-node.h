#ifndef HTML_NODE_H
#define HTML_NODE_H

#include <memory>
#include <QList>
#include <QString>
#include "lexbor/html/html.h"


class HtmlNode
{
	public:
		HtmlNode(lxb_dom_node_t *node, std::shared_ptr<lxb_html_document_t> document);
		static HtmlNode *fromString(const QString &html, bool fragment = false);
		~HtmlNode() = default;

		QString outerHTML() const;
		QString innerHTML() const;
		QString innerText() const;

		QString tag() const;
		QString attr(const QString &attr) const;
		QStringList path() const;
		QStringList pathIds() const;

		HtmlNode parent() const;
		QList<HtmlNode> find(const QString &css) const;

	private:
		lxb_dom_node_t *m_node;
		std::shared_ptr<lxb_html_document_t> m_document;
};

#endif // HTML_NODE_H
