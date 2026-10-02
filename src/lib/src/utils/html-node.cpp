#include "html-node.h"
#include <QByteArray>
#include <utility>
#include "lexbor/css/css.h"
#include "lexbor/html/html.h"
#include "lexbor/selectors/selectors.h"
#include "logger.h"


HtmlNode::HtmlNode(lxb_dom_node_t *node, std::shared_ptr<lxb_html_document_t> document)
	: m_node(node), m_document(std::move(document))
{}

HtmlNode *HtmlNode::fromString(const QString &html, bool fragment)
{
	const std::shared_ptr<lxb_html_document_t> document(lxb_html_document_create(), lxb_html_document_destroy);
	if (!document) {
		return nullptr;
	}
	const QByteArray bytes = html.toUtf8();

	// Parse an HTML fragment
	if (fragment) {
		const QString fragmentWrapper = "p";
		auto *root = lxb_html_document_create_element(document.get(), reinterpret_cast<const lxb_char_t *>(fragmentWrapper.toStdString().c_str()), fragmentWrapper.length(), NULL);
		if (root == nullptr) {
			return nullptr;
		}
		auto *node = lxb_html_document_parse_fragment(document.get(), &root->element, reinterpret_cast<const lxb_char_t *>(bytes.constData()), bytes.size());

		// Ignore the default-built "html" node if there's only one child
		if (node != NULL && node->first_child == node->last_child) {
			node = node->first_child;
		}

		// If no node is found, that means an error occurred
		if (node == NULL) {
			log(QStringLiteral("Error parsing HTML fragment."), Logger::Error);
			return nullptr;
		}

		return new HtmlNode(node, document);
	}

	// Parse a whole HTML document
	auto status = lxb_html_document_parse(document.get(), reinterpret_cast<const lxb_char_t *>(bytes.constData()), bytes.size());
	if (status != LXB_STATUS_OK) {
		log(QStringLiteral("Error parsing HTML: %1.").arg(status), Logger::Error);
		return nullptr;
	}
	const auto *body = lxb_html_document_body_element(document.get());
	return new HtmlNode(lxb_dom_interface_node(body), document);
}


QString HtmlNode::outerHTML() const
{
	if (m_node == nullptr) {
		return {};
	}
	lexbor_str_t str = {NULL};
	auto status = lxb_html_serialize_tree_str(m_node, &str);
	if (status != LXB_STATUS_OK) {
		lexbor_str_destroy(&str, m_node->owner_document->text, false);
		log(QStringLiteral("Error serializing HTML node: %1.").arg(status), Logger::Error);
		return {};
	}
	const QString result = QString::fromUtf8(reinterpret_cast<const char *>(str.data), str.length);
	lexbor_str_destroy(&str, m_node->owner_document->text, false);
	return result;
}

QString HtmlNode::innerHTML() const
{
	if (m_node == nullptr) {
		return {};
	}
	lexbor_str_t str = {NULL};
	auto status = lxb_html_serialize_deep_str(m_node, &str);
	if (status != LXB_STATUS_OK) {
		lexbor_str_destroy(&str, m_node->owner_document->text, false);
		log(QStringLiteral("Error serializing HTML node: %1.").arg(status), Logger::Error);
		return {};
	}
	const QString result = QString::fromUtf8(reinterpret_cast<const char *>(str.data), str.length);
	lexbor_str_destroy(&str, m_node->owner_document->text, false);
	return result;
}

QString HtmlNode::innerText() const
{
	if (m_node == nullptr) {
		return {};
	}
	size_t length = 0;
	lxb_char_t *str = lxb_dom_node_text_content(m_node, &length);
	const QString result = QString::fromUtf8(reinterpret_cast<const char *>(str), length);
	lxb_dom_document_destroy_text(m_node->owner_document, str);
	return result;
}


QString HtmlNode::tag() const
{
	if (m_node == nullptr || m_node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
		return {};
	}
	const auto *tagName = lxb_dom_element_qualified_name(lxb_dom_interface_element(m_node), NULL);
	return QString(reinterpret_cast<const char *>(tagName));
}

QString HtmlNode::attr(const QString &attr) const
{
	if (m_node == nullptr || m_node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
		return {};
	}
	const QByteArray name = attr.toUtf8();
	auto *element = lxb_dom_interface_element(m_node);

	// Check that attribute exists
	bool is_exist = lxb_dom_element_has_attribute(element, reinterpret_cast<const lxb_char_t *>(name.constData()), name.size());
	if (!is_exist) {
		return {};
	}

	// Get attribute value from DOM
	const lxb_char_t *value = lxb_dom_element_get_attribute(element, reinterpret_cast<const lxb_char_t *>(name.constData()), name.size(), NULL);
	if (value == NULL) {
		log(QStringLiteral("Error getting attribute: %1.").arg(attr), Logger::Error);
		return {};
	}

	return QString(reinterpret_cast<const char *>(value));
}

QStringList HtmlNode::path() const
{
	QStringList ret;

	lxb_dom_node_t *parent = m_node != nullptr ? m_node->parent : nullptr;
	while (parent != nullptr && parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
		const auto *tagName = lxb_dom_element_qualified_name(lxb_dom_interface_element(parent), NULL);
		ret.prepend(QString(reinterpret_cast<const char *>(tagName)));

		parent = parent->parent;
	}

	return ret;
}

QStringList HtmlNode::pathIds() const
{
	QStringList ret;

	lxb_dom_node_t *parent = m_node != nullptr ? m_node->parent : nullptr;
	while (parent != nullptr && parent->parent != nullptr) {
		ret.prepend(QString::number((quintptr) parent));
		parent = parent->parent;
	}

	return ret;
}


lxb_status_t find_callback(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
	Q_UNUSED(spec)

	auto *ret = static_cast<QList<lxb_dom_node_t*>*>(ctx);
	ret->append(node);
	return LXB_STATUS_OK;
}

HtmlNode HtmlNode::parent() const
{
	return HtmlNode(m_node != nullptr ? m_node->parent : nullptr, m_document);
}

QList<HtmlNode> HtmlNode::find(const QString &css) const
{
	if (m_node == nullptr) {
		return {};
	}
	// Keep both parser resources and selector allocations bounded to this query.
	auto destroyParser = [](lxb_css_parser_t *parser) {
		if (parser != nullptr) {
			auto *memory = parser->memory;
			lxb_css_parser_destroy(parser, true);
			lxb_css_memory_destroy(memory, true);
		}
	};
	const std::unique_ptr<lxb_css_parser_t, decltype(destroyParser)> parser(lxb_css_parser_create(), destroyParser);
	auto parser_status = lxb_css_parser_init(parser.get(), NULL);
	if (parser_status != LXB_STATUS_OK) {
		log(QStringLiteral("Error creating CSS parser: %1.").arg(parser_status), Logger::Error);
		return {};
	}

	// Create CSS selectors
	auto destroySelectors = [](lxb_selectors_t *selectors) { lxb_selectors_destroy(selectors, true); };
	const std::unique_ptr<lxb_selectors_t, decltype(destroySelectors)> selectors(lxb_selectors_create(), destroySelectors);
	auto selectors_status = lxb_selectors_init(selectors.get());
	if (selectors_status != LXB_STATUS_OK) {
		log(QStringLiteral("Error creating CSS selectors: %1.").arg(selectors_status), Logger::Error);
		return {};
	}

	// Parse CSS selectors
	const QByteArray query = css.toUtf8();
	auto *list = lxb_css_selectors_parse(parser.get(), reinterpret_cast<const lxb_char_t *>(query.constData()), query.size());
	if (parser->status != LXB_STATUS_OK) {
		log(QStringLiteral("Error parsing CSS selectors: %1.").arg(parser->status), Logger::Error);
		return {};
	}

	// Find matching HTML nodes
	QList<lxb_dom_node_t*> found;
	auto status_find = lxb_selectors_find(selectors.get(), m_node, list, find_callback, &found);
	if (status_find != LXB_STATUS_OK) {
		log(QStringLiteral("Error finding nodes via CSS selectors: %1.").arg(status_find), Logger::Error);
		return {};
	}

	QList<HtmlNode> nodes;
	for (auto *node : found) {
		nodes.append(HtmlNode(node, m_document));
	}
	return nodes;
}
