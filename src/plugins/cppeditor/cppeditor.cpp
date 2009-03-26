/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact:  Qt Software Information (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at qt-sales@nokia.com.
**
**************************************************************************/

#include "cppeditor.h"
#include "cppeditorconstants.h"
#include "cppplugin.h"
#include "cpphighlighter.h"

#include <AST.h>
#include <Token.h>
#include <Scope.h>
#include <Symbols.h>
#include <Names.h>
#include <Control.h>
#include <CoreTypes.h>
#include <Literals.h>
#include <Semantic.h>
#include <SymbolVisitor.h>
#include <cplusplus/ExpressionUnderCursor.h>
#include <cplusplus/LookupContext.h>
#include <cplusplus/Overview.h>
#include <cplusplus/OverviewModel.h>
#include <cplusplus/SimpleLexer.h>
#include <cplusplus/TokenUnderCursor.h>
#include <cplusplus/TypeOfExpression.h>
#include <cpptools/cppmodelmanagerinterface.h>

#include <coreplugin/icore.h>
#include <coreplugin/uniqueidmanager.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/editormanager/editormanager.h>
#include <extensionsystem/pluginmanager.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <texteditor/basetextdocument.h>
#include <texteditor/fontsettings.h>
#include <texteditor/texteditorconstants.h>
#include <texteditor/textblockiterator.h>
#include <indenter.h>

#include <QtCore/QDebug>
#include <QtCore/QTime>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QHeaderView>
#include <QtGui/QLayout>
#include <QtGui/QMenu>
#include <QtGui/QShortcut>
#include <QtGui/QTextEdit>
#include <QtGui/QComboBox>
#include <QtGui/QTreeView>

using namespace CPlusPlus;
using namespace CppEditor::Internal;

namespace {

class OverviewTreeView : public QTreeView
{
public:
    OverviewTreeView(QWidget *parent = 0)
        : QTreeView(parent)
    {
        // TODO: Disable the root for all items (with a custom delegate?)
        setRootIsDecorated(false);
    }

    void sync()
    {
        expandAll();
        setMinimumWidth(qMax(sizeHintForColumn(0), minimumSizeHint().width()));
    }
};

class FindFunctionDefinitions: protected SymbolVisitor
{
    Name *_declarationName;
    QList<Function *> *_functions;

public:
    FindFunctionDefinitions()
        : _declarationName(0),
          _functions(0)
    { }

    void operator()(Name *declarationName, Scope *globals,
                    QList<Function *> *functions)
    {
        _declarationName = declarationName;
        _functions = functions;

        for (unsigned i = 0; i < globals->symbolCount(); ++i) {
            accept(globals->symbolAt(i));
        }
    }

protected:
    using SymbolVisitor::visit;

    virtual bool visit(Function *function)
    {
        Name *name = function->name();
        if (QualifiedNameId *q = name->asQualifiedNameId())
            name = q->unqualifiedNameId();

        if (_declarationName->isEqualTo(name))
            _functions->append(function);

        return false;
    }
};

} // end of anonymous namespace

static QualifiedNameId *qualifiedNameIdForSymbol(Symbol *s, const LookupContext &context)
{
    Name *symbolName = s->name();
    if (! symbolName)
        return 0; // nothing to do.

    QVector<Name *> names;

    for (Scope *scope = s->scope(); scope; scope = scope->enclosingScope()) {
        if (scope->isClassScope() || scope->isNamespaceScope()) {
            if (scope->owner() && scope->owner()->name()) {
                Name *ownerName = scope->owner()->name();
                if (QualifiedNameId *q = ownerName->asQualifiedNameId()) {
                    for (unsigned i = 0; i < q->nameCount(); ++i) {
                        names.prepend(q->nameAt(i));
                    }
                } else {
                    names.prepend(ownerName);
                }
            }
        }
    }

    if (QualifiedNameId *q = symbolName->asQualifiedNameId()) {
        for (unsigned i = 0; i < q->nameCount(); ++i) {
            names.append(q->nameAt(i));
        }
    } else {
        names.append(symbolName);
    }

    return context.control()->qualifiedNameId(names.constData(), names.size());
}

CPPEditorEditable::CPPEditorEditable(CPPEditor *editor)
    : BaseTextEditorEditable(editor)
{
    Core::UniqueIDManager *uidm = Core::UniqueIDManager::instance();
    m_context << uidm->uniqueIdentifier(CppEditor::Constants::C_CPPEDITOR);
    m_context << uidm->uniqueIdentifier(ProjectExplorer::Constants::LANG_CXX);
    m_context << uidm->uniqueIdentifier(TextEditor::Constants::C_TEXTEDITOR);
}

CPPEditor::CPPEditor(QWidget *parent)
    : TextEditor::BaseTextEditor(parent)
    , m_showingLink(false)
{
    setParenthesesMatchingEnabled(true);
    setMarksVisible(true);
    setCodeFoldingSupported(true);
    setCodeFoldingVisible(true);
    baseTextDocument()->setSyntaxHighlighter(new CppHighlighter);

#ifdef WITH_TOKEN_MOVE_POSITION
    new QShortcut(QKeySequence::MoveToPreviousWord, this, SLOT(moveToPreviousToken()),
                  /*ambiguousMember=*/ 0, Qt::WidgetShortcut);

    new QShortcut(QKeySequence::MoveToNextWord, this, SLOT(moveToNextToken()),
                  /*ambiguousMember=*/ 0, Qt::WidgetShortcut);

    new QShortcut(QKeySequence::DeleteStartOfWord, this, SLOT(deleteStartOfToken()),
                  /*ambiguousMember=*/ 0, Qt::WidgetShortcut);

    new QShortcut(QKeySequence::DeleteEndOfWord, this, SLOT(deleteEndOfToken()),
                  /*ambiguousMember=*/ 0, Qt::WidgetShortcut);
#endif

    m_modelManager = ExtensionSystem::PluginManager::instance()
        ->getObject<CppTools::CppModelManagerInterface>();

    if (m_modelManager) {
        connect(m_modelManager, SIGNAL(documentUpdated(CPlusPlus::Document::Ptr)),
                this, SLOT(onDocumentUpdated(CPlusPlus::Document::Ptr)));
    }
}

CPPEditor::~CPPEditor()
{
}

TextEditor::BaseTextEditorEditable *CPPEditor::createEditableInterface()
{
    CPPEditorEditable *editable = new CPPEditorEditable(this);
    createToolBar(editable);
    return editable;
}

void CPPEditor::createToolBar(CPPEditorEditable *editable)
{
    m_methodCombo = new QComboBox;
    m_methodCombo->setMinimumContentsLength(22);
    m_methodCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    // Make the combo box prefer to expand
    QSizePolicy policy = m_methodCombo->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    m_methodCombo->setSizePolicy(policy);

    QTreeView *methodView = new OverviewTreeView;
    methodView->header()->hide();
    methodView->setItemsExpandable(false);
    m_methodCombo->setView(methodView);
    m_methodCombo->setMaxVisibleItems(20);

    m_overviewModel = new OverviewModel(this);
    m_methodCombo->setModel(m_overviewModel);

    connect(m_methodCombo, SIGNAL(activated(int)), this, SLOT(jumpToMethod(int)));
    connect(this, SIGNAL(cursorPositionChanged()), this, SLOT(updateMethodBoxIndex()));
    connect(m_methodCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(updateMethodBoxToolTip()));

    connect(file(), SIGNAL(changed()), this, SLOT(updateFileName()));

    QToolBar *toolBar = editable->toolBar();
    QList<QAction*> actions = toolBar->actions();
    toolBar->insertWidget(actions.first(), m_methodCombo);
}

int CPPEditor::previousBlockState(QTextBlock block) const
{
    block = block.previous();
    if (block.isValid()) {
        int state = block.userState();

        if (state != -1)
            return state;
    }
    return 0;
}

QTextCursor CPPEditor::moveToPreviousToken(QTextCursor::MoveMode mode) const
{
    SimpleLexer tokenize;
    QTextCursor c(textCursor());
    QTextBlock block = c.block();
    int column = c.columnNumber();

    for (; block.isValid(); block = block.previous()) {
        const QString textBlock = block.text();
        QList<SimpleToken> tokens = tokenize(textBlock, previousBlockState(block));

        if (! tokens.isEmpty()) {
            tokens.prepend(SimpleToken());

            for (int index = tokens.size() - 1; index != -1; --index) {
                const SimpleToken &tk = tokens.at(index);
                if (tk.position() < column) {
                    c.setPosition(block.position() + tk.position(), mode);
                    return c;
                }
            }
        }

        column = INT_MAX;
    }

    c.movePosition(QTextCursor::Start, mode);
    return c;
}

QTextCursor CPPEditor::moveToNextToken(QTextCursor::MoveMode mode) const
{
    SimpleLexer tokenize;
    QTextCursor c(textCursor());
    QTextBlock block = c.block();
    int column = c.columnNumber();

    for (; block.isValid(); block = block.next()) {
        const QString textBlock = block.text();
        QList<SimpleToken> tokens = tokenize(textBlock, previousBlockState(block));

        if (! tokens.isEmpty()) {
            for (int index = 0; index < tokens.size(); ++index) {
                const SimpleToken &tk = tokens.at(index);
                if (tk.position() > column) {
                    c.setPosition(block.position() + tk.position(), mode);
                    return c;
                }
            }
        }

        column = -1;
    }

    c.movePosition(QTextCursor::End, mode);
    return c;
}

void CPPEditor::moveToPreviousToken()
{
    setTextCursor(moveToPreviousToken(QTextCursor::MoveAnchor));
}

void CPPEditor::moveToNextToken()
{
    setTextCursor(moveToNextToken(QTextCursor::MoveAnchor));
}

void CPPEditor::deleteStartOfToken()
{
    QTextCursor c = moveToPreviousToken(QTextCursor::KeepAnchor);
    c.removeSelectedText();
    setTextCursor(c);
}

void CPPEditor::deleteEndOfToken()
{
    QTextCursor c = moveToNextToken(QTextCursor::KeepAnchor);
    c.removeSelectedText();
    setTextCursor(c);
}

void CPPEditor::onDocumentUpdated(Document::Ptr doc)
{
    if (doc->fileName() != file()->fileName())
        return;

    m_overviewModel->rebuild(doc);
    OverviewTreeView *treeView = static_cast<OverviewTreeView *>(m_methodCombo->view());
    treeView->sync();
    updateMethodBoxIndex();
}

void CPPEditor::updateFileName()
{ }

void CPPEditor::jumpToMethod(int)
{
    QModelIndex index = m_methodCombo->view()->currentIndex();
    Symbol *symbol = m_overviewModel->symbolFromIndex(index);
    if (! symbol)
        return;

    openCppEditorAt(linkToSymbol(symbol));
}

void CPPEditor::updateMethodBoxIndex()
{
    int line = 0, column = 0;
    convertPosition(position(), &line, &column);

    QModelIndex lastIndex;

    const int rc = m_overviewModel->rowCount();
    for (int row = 0; row < rc; ++row) {
        const QModelIndex index = m_overviewModel->index(row, 0, QModelIndex());
        Symbol *symbol = m_overviewModel->symbolFromIndex(index);
        if (symbol && symbol->line() > unsigned(line))
            break;
        lastIndex = index;
    }

    QList<QTextEdit::ExtraSelection> selections;

    if (lastIndex.isValid()) {
        bool blocked = m_methodCombo->blockSignals(true);
        m_methodCombo->setCurrentIndex(lastIndex.row());
        updateMethodBoxToolTip();
        (void) m_methodCombo->blockSignals(blocked);
    }

#ifdef QTCREATOR_WITH_ADVANCED_HIGHLIGHTER
    Snapshot snapshot = m_modelManager->snapshot();
    Document::Ptr thisDocument = snapshot.value(file()->fileName());
    if (! thisDocument)
        return;

    if (Symbol *symbol = thisDocument->findSymbolAt(line, column)) {
        QTextCursor tc = textCursor();
        tc.movePosition(QTextCursor::EndOfWord);

        ExpressionUnderCursor expressionUnderCursor;

        const QString expression = expressionUnderCursor(tc);
        //qDebug() << "expression:" << expression;

        Snapshot snapshot;
        snapshot.insert(thisDocument->fileName(), thisDocument);

        TypeOfExpression typeOfExpression;
        typeOfExpression.setSnapshot(snapshot);

        const QList<TypeOfExpression::Result> results =
                typeOfExpression(expression, thisDocument, symbol, TypeOfExpression::Preprocess);

        LookupContext context = typeOfExpression.lookupContext();

        foreach (const TypeOfExpression::Result &result, results) {
            FullySpecifiedType ty = result.first;
            Symbol *symbol = result.second;

            if (file()->fileName() != symbol->fileName())
                continue;

            else if (symbol->isGenerated())
                continue;

            else if (symbol->isBlock())
                continue;

            if (symbol) {
                int column = symbol->column();

                if (column != 0)
                    --column;

                if (symbol->isGenerated())
                    column = 0;

                QTextCursor c(document()->findBlockByNumber(symbol->line() - 1));
                c.setPosition(c.position() + column);

                const QString text = c.block().text();

                int i = column;
                for (; i < text.length(); ++i) {
                    const QChar &ch = text.at(i);

                    if (ch == QLatin1Char('*') || ch == QLatin1Char('&'))
                        c.movePosition(QTextCursor::NextCharacter);
                    else
                        break;
                }

                c.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);

                QTextEdit::ExtraSelection sel;
                sel.cursor = c;
                sel.format.setBackground(Qt::darkYellow);

                selections.append(sel);
                //break;
            }
        }
    }

    setExtraSelections(CodeSemanticsSelection, selections);
#endif // QTCREATOR_WITH_ADVANCED_HIGHLIGHTER
}

void CPPEditor::updateMethodBoxToolTip()
{
    m_methodCombo->setToolTip(m_methodCombo->currentText());
}

static bool isCompatible(Name *name, Name *otherName)
{
    if (NameId *nameId = name->asNameId()) {
        if (TemplateNameId *otherTemplId = otherName->asTemplateNameId())
            return nameId->identifier()->isEqualTo(otherTemplId->identifier());
    } else if (TemplateNameId *templId = name->asTemplateNameId()) {
        if (NameId *otherNameId = otherName->asNameId())
            return templId->identifier()->isEqualTo(otherNameId->identifier());
    }

    return name->isEqualTo(otherName);
}

static bool isCompatible(Function *definition, Symbol *declaration, QualifiedNameId *declarationName)
{
    Function *declTy = declaration->type()->asFunctionType();
    if (! declTy)
        return false;

    Name *definitionName = definition->name();
    if (QualifiedNameId *q = definitionName->asQualifiedNameId()) {
        if (! isCompatible(q->unqualifiedNameId(), declaration->name()))
            return false;
        else if (q->nameCount() > declarationName->nameCount())
            return false;
        else if (declTy->argumentCount() != definition->argumentCount())
            return false;
        else if (declTy->isConst() != definition->isConst())
            return false;
        else if (declTy->isVolatile() != definition->isVolatile())
            return false;

        for (unsigned i = 0; i < definition->argumentCount(); ++i) {
            Symbol *arg = definition->argumentAt(i);
            Symbol *otherArg = declTy->argumentAt(i);
            if (! arg->type().isEqualTo(otherArg->type()))
                return false;
        }

        for (unsigned i = 0; i != q->nameCount(); ++i) {
            Name *n = q->nameAt(q->nameCount() - i - 1);
            Name *m = declarationName->nameAt(declarationName->nameCount() - i - 1);
            if (! isCompatible(n, m))
                return false;
        }
        return true;
    } else {
        // ### TODO: implement isCompatible for unqualified name ids.
    }
    return false;
}

void CPPEditor::switchDeclarationDefinition()
{
    int line = 0, column = 0;
    convertPosition(position(), &line, &column);

    if (!m_modelManager)
        return;

    const Snapshot snapshot = m_modelManager->snapshot();

    Document::Ptr doc = snapshot.value(file()->fileName());
    if (!doc)
        return;
    Symbol *lastSymbol = doc->findSymbolAt(line, column);
    if (!lastSymbol || !lastSymbol->scope())
        return;

    Function *f = lastSymbol->asFunction();
    if (!f) {
        Scope *fs = lastSymbol->scope();
        if (!fs->isFunctionScope())
            fs = fs->enclosingFunctionScope();
        if (fs)
            f = fs->owner()->asFunction();
    }

    if (f) {
        TypeOfExpression typeOfExpression;
        typeOfExpression.setSnapshot(m_modelManager->snapshot());
        QList<TypeOfExpression::Result> resolvedSymbols = typeOfExpression(QString(), doc, lastSymbol);
        const LookupContext &context = typeOfExpression.lookupContext();

        QualifiedNameId *q = qualifiedNameIdForSymbol(f, context);
        QList<Symbol *> symbols = context.resolve(q);

        Symbol *declaration = 0;
        foreach (declaration, symbols) {
            if (isCompatible(f, declaration, q))
                break;
        }

        if (! declaration && ! symbols.isEmpty())
            declaration = symbols.first();

        if (declaration)
            openCppEditorAt(linkToSymbol(declaration));
    } else if (lastSymbol->type()->isFunctionType()) {
        if (Symbol *def = findDefinition(lastSymbol))
            openCppEditorAt(linkToSymbol(def));
    }
}

CPPEditor::Link CPPEditor::findLinkAt(const QTextCursor &cursor,
                                      bool lookupDefinition)
{
    Link link;

    if (!m_modelManager)
        return link;

    const Snapshot snapshot = m_modelManager->snapshot();
    int line = 0, column = 0;
    convertPosition(cursor.position(), &line, &column);
    Document::Ptr doc = snapshot.value(file()->fileName());
    if (!doc)
        return link;

    // Handle include directives
    const unsigned lineno = cursor.blockNumber() + 1;
    foreach (const Document::Include &incl, doc->includes()) {
        if (incl.line() == lineno) {
            link.fileName = incl.fileName();
            link.pos = cursor.block().position();
            link.length = cursor.block().length();
            return link;
        }
    }

    // Find the last symbol up to the cursor position
    Symbol *lastSymbol = doc->findSymbolAt(line, column);
    if (!lastSymbol)
        return link;

    // Check whether we're at a name
    const int endOfName = endOfNameAtPosition(cursor.position());
    if (!characterAt(endOfName - 1).isLetterOrNumber())
        return link;

    // Remember the position and length of the name
    QTextCursor tc = cursor;
    tc.setPosition(endOfName);
    tc.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
    const int nameStart = tc.position();
    const int nameLength = tc.anchor() - tc.position();
    tc.setPosition(endOfName);

    // Drop out if we're at a number, string or comment
    static TokenUnderCursor tokenUnderCursor;
    const SimpleToken tk = tokenUnderCursor(tc);
    if (tk.isLiteral() || tk.isComment())
        return link;

    // Evaluate the type of the expression under the cursor
    ExpressionUnderCursor expressionUnderCursor;
    const QString expression = expressionUnderCursor(tc);
    TypeOfExpression typeOfExpression;
    typeOfExpression.setSnapshot(snapshot);
    QList<TypeOfExpression::Result> resolvedSymbols =
            typeOfExpression(expression, doc, lastSymbol);

    if (!resolvedSymbols.isEmpty()) {
        TypeOfExpression::Result result = resolvedSymbols.first();

        if (result.first->isForwardClassDeclarationType()) {
            while (! resolvedSymbols.isEmpty()) {
                TypeOfExpression::Result r = resolvedSymbols.takeFirst();

                if (! r.first->isForwardClassDeclarationType()) {
                    result = r;
                    break;
                }
            }
        }

        if (Symbol *symbol = result.second) {
            Symbol *def = 0;
            if (lookupDefinition && !lastSymbol->isFunction())
                def = findDefinition(symbol);

            link = linkToSymbol(def ? def : symbol);
            link.pos = nameStart;
            link.length = nameLength;
            return link;

        // This would jump to the type of a name
#if 0
        } else if (NamedType *namedType = firstType->asNamedType()) {
            QList<Symbol *> candidates = context.resolve(namedType->name());
            if (!candidates.isEmpty()) {
                Symbol *s = candidates.takeFirst();
                openCppEditorAt(s->fileName(), s->line(), s->column());
            }
#endif
        }
    } else {
        // Handle macro uses
        foreach (const Document::MacroUse use, doc->macroUses()) {
            if (use.contains(endOfName - 1)) {
                const Macro &macro = use.macro();
                link.fileName = QString::fromUtf8(macro.fileName());
                link.line = macro.line();
                link.pos = use.begin();
                link.length = use.end() - use.begin();
                return link;
            }
        }
    }

    return link;
}

void CPPEditor::jumpToDefinition()
{
    openCppEditorAt(findLinkAt(textCursor()));
}

Symbol *CPPEditor::findDefinition(Symbol *symbol)
{
    if (symbol->isFunction())
        return 0; // symbol is a function definition.

    Function *funTy = symbol->type()->asFunctionType();
    if (! funTy)
        return 0; // symbol does not have function type.

    Name *name = symbol->name();
    if (! name)
        return 0; // skip anonymous functions!

    if (QualifiedNameId *q = name->asQualifiedNameId())
        name = q->unqualifiedNameId();

    // map from file names to function definitions.
    QMap<QString, QList<Function *> > functionDefinitions;

    // find function definitions.
    FindFunctionDefinitions findFunctionDefinitions;

    // save the current snapshot
    const Snapshot snapshot = m_modelManager->snapshot();

    foreach (Document::Ptr doc, snapshot) {
        if (Scope *globals = doc->globalSymbols()) {
            QList<Function *> *localFunctionDefinitions =
                    &functionDefinitions[doc->fileName()];

            findFunctionDefinitions(name, globals,
                                    localFunctionDefinitions);
        }
    }

    // a dummy document.
    Document::Ptr expressionDocument = Document::create("<empty>");

    QMapIterator<QString, QList<Function *> > it(functionDefinitions);
    while (it.hasNext()) {
        it.next();

        // get the instance of the document.
        Document::Ptr thisDocument = snapshot.value(it.key());

        foreach (Function *f, it.value()) {
            // create a lookup context
            const LookupContext context(f, expressionDocument,
                                        thisDocument, snapshot);

            // search the matching definition for the function declaration `symbol'.
            foreach (Symbol *s, context.resolve(f->name())) {
                if (s == symbol)
                    return f;
            }
        }
    }

    return 0;
}

bool CPPEditor::isElectricCharacter(const QChar &ch) const
{
    if (ch == QLatin1Char('{') ||
        ch == QLatin1Char('}') ||
        ch == QLatin1Char('#')) {
        return true;
    }
    return false;
}

// Indent a code line based on previous
template <class Iterator>
static void indentCPPBlock(const CPPEditor::TabSettings &ts,
                           const QTextBlock &block,
                           const Iterator &programBegin,
                           const Iterator &programEnd,
                           QChar typedChar)
{
    typedef typename SharedTools::Indenter<Iterator> Indenter;
    Indenter &indenter = Indenter::instance();
    indenter.setIndentSize(ts.m_indentSize);
    indenter.setTabSize(ts.m_tabSize);

    const TextEditor::TextBlockIterator current(block);
    const int indent = indenter.indentForBottomLine(current, programBegin, programEnd, typedChar);
    ts.indentLine(block, indent);
}

void CPPEditor::indentBlock(QTextDocument *doc, QTextBlock block, QChar typedChar)
{
    const TextEditor::TextBlockIterator begin(doc->begin());
    const TextEditor::TextBlockIterator end(block.next());

    indentCPPBlock(tabSettings(), block, begin, end, typedChar);
}

void CPPEditor::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu *menu = createStandardContextMenu();

    // Remove insert unicode control character
    QAction *lastAction = menu->actions().last();
    if (lastAction->menu() && QLatin1String(lastAction->menu()->metaObject()->className()) == QLatin1String("QUnicodeControlCharacterMenu"))
        menu->removeAction(lastAction);

    Core::ActionContainer *mcontext =
        Core::ICore::instance()->actionManager()->actionContainer(CppEditor::Constants::M_CONTEXT);
    QMenu *contextMenu = mcontext->menu();

    foreach (QAction *action, contextMenu->actions())
        menu->addAction(action);

    menu->exec(e->globalPos());
    delete menu;
}

void CPPEditor::mouseMoveEvent(QMouseEvent *e)
{
    bool linkFound = false;

    if (e->modifiers() & Qt::ControlModifier) {
        // Link emulation behaviour for 'go to definition'
        const QTextCursor cursor = cursorForPosition(e->pos());

        // Check that the mouse was actually on the text somewhere
        bool onText = cursorRect(cursor).right() >= e->x();
        if (!onText) {
            QTextCursor nextPos = cursor;
            nextPos.movePosition(QTextCursor::Right);
            onText = cursorRect(nextPos).right() >= e->x();
        }

        const Link link = findLinkAt(cursor, false);

        if (onText && !link.fileName.isEmpty()) {
            showLink(link);
            linkFound = true;
        }
    }

    if (!linkFound)
        clearLink();

    TextEditor::BaseTextEditor::mouseMoveEvent(e);
}

void CPPEditor::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier && !(e->modifiers() & Qt::ShiftModifier)
        && e->button() == Qt::LeftButton) {

        const QTextCursor cursor = cursorForPosition(e->pos());
        if (openCppEditorAt(findLinkAt(cursor))) {
            clearLink();
            e->accept();
            return;
        }
    }

    TextEditor::BaseTextEditor::mouseReleaseEvent(e);
}

void CPPEditor::leaveEvent(QEvent *e)
{
    clearLink();
    TextEditor::BaseTextEditor::leaveEvent(e);
}

void CPPEditor::keyReleaseEvent(QKeyEvent *e)
{
    // Clear link emulation when Ctrl is released
    if (e->key() == Qt::Key_Control)
        clearLink();

    TextEditor::BaseTextEditor::keyReleaseEvent(e);
}

void CPPEditor::showLink(const Link &link)
{
    QTextEdit::ExtraSelection sel;
    sel.cursor = textCursor();
    sel.cursor.setPosition(link.pos);
    sel.cursor.setPosition(link.pos + link.length, QTextCursor::KeepAnchor);
    sel.format = m_linkFormat;
    sel.format.setFontUnderline(true);
    setExtraSelections(OtherSelection, QList<QTextEdit::ExtraSelection>() << sel);
    viewport()->setCursor(Qt::PointingHandCursor);
    m_showingLink = true;
}

void CPPEditor::clearLink()
{
    if (!m_showingLink)
        return;

    setExtraSelections(OtherSelection, QList<QTextEdit::ExtraSelection>());
    viewport()->setCursor(Qt::IBeamCursor);
    m_showingLink = false;
}

QList<int> CPPEditorEditable::context() const
{
    return m_context;
}

Core::IEditor *CPPEditorEditable::duplicate(QWidget *parent)
{
    CPPEditor *newEditor = new CPPEditor(parent);
    newEditor->duplicateFrom(editor());
    CppPlugin::instance()->initializeEditor(newEditor);
    return newEditor->editableInterface();
}

const char *CPPEditorEditable::kind() const
{
    return CppEditor::Constants::CPPEDITOR_KIND;
}

void CPPEditor::setFontSettings(const TextEditor::FontSettings &fs)
{
    TextEditor::BaseTextEditor::setFontSettings(fs);
    CppHighlighter *highlighter = qobject_cast<CppHighlighter*>(baseTextDocument()->syntaxHighlighter());
    if (!highlighter)
        return;

    static QVector<QString> categories;
    if (categories.isEmpty()) {
        categories << QLatin1String(TextEditor::Constants::C_NUMBER)
                   << QLatin1String(TextEditor::Constants::C_STRING)
                   << QLatin1String(TextEditor::Constants::C_TYPE)
                   << QLatin1String(TextEditor::Constants::C_KEYWORD)
                   << QLatin1String(TextEditor::Constants::C_OPERATOR)
                   << QLatin1String(TextEditor::Constants::C_PREPROCESSOR)
                   << QLatin1String(TextEditor::Constants::C_LABEL)
                   << QLatin1String(TextEditor::Constants::C_COMMENT)
                   << QLatin1String(TextEditor::Constants::C_DOXYGEN_COMMENT)
                   << QLatin1String(TextEditor::Constants::C_DOXYGEN_TAG);
    }

    const QVector<QTextCharFormat> formats = fs.toTextCharFormats(categories);
    highlighter->setFormats(formats.constBegin(), formats.constEnd());
    highlighter->rehighlight();

    m_linkFormat = fs.toTextCharFormat(QLatin1String(TextEditor::Constants::C_LINK));
}


void CPPEditor::unCommentSelection()
{
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();

    int pos = cursor.position();
    int anchor = cursor.anchor();
    int start = qMin(anchor, pos);
    int end = qMax(anchor, pos);
    bool anchorIsStart = (anchor == start);

    QTextBlock startBlock = document()->findBlock(start);
    QTextBlock endBlock = document()->findBlock(end);

    if (end > start && endBlock.position() == end) {
        --end;
        endBlock = endBlock.previous();
    }

    bool doCStyleUncomment = false;
    bool doCStyleComment = false;
    bool doCppStyleUncomment = false;

    bool hasSelection = cursor.hasSelection();

    if (hasSelection) {
        QString startText = startBlock.text();
        int startPos = start - startBlock.position();
        bool hasLeadingCharacters = !startText.left(startPos).trimmed().isEmpty();
        if ((startPos >= 2
            && startText.at(startPos-2) == QLatin1Char('/')
             && startText.at(startPos-1) == QLatin1Char('*'))) {
            startPos -= 2;
            start -= 2;
        }

        bool hasSelStart = (startPos < startText.length() - 2
                            && startText.at(startPos) == QLatin1Char('/')
                            && startText.at(startPos+1) == QLatin1Char('*'));


        QString endText = endBlock.text();
        int endPos = end - endBlock.position();
        bool hasTrailingCharacters = !endText.left(endPos).remove(QLatin1String("//")).trimmed().isEmpty()
                                     && !endText.mid(endPos).trimmed().isEmpty();
        if ((endPos <= endText.length() - 2
            && endText.at(endPos) == QLatin1Char('*')
             && endText.at(endPos+1) == QLatin1Char('/'))) {
            endPos += 2;
            end += 2;
        }

        bool hasSelEnd = (endPos >= 2
                          && endText.at(endPos-2) == QLatin1Char('*')
                          && endText.at(endPos-1) == QLatin1Char('/'));

        doCStyleUncomment = hasSelStart && hasSelEnd;
        doCStyleComment = !doCStyleUncomment && (hasLeadingCharacters || hasTrailingCharacters);
    }

    if (doCStyleUncomment) {
        cursor.setPosition(end);
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, 2);
        cursor.removeSelectedText();
        cursor.setPosition(start);
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 2);
        cursor.removeSelectedText();
    } else if (doCStyleComment) {
        cursor.setPosition(end);
        cursor.insertText(QLatin1String("*/"));
        cursor.setPosition(start);
        cursor.insertText(QLatin1String("/*"));
    } else {
        endBlock = endBlock.next();
        doCppStyleUncomment = true;
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            QString text = block.text();
            if (!text.trimmed().startsWith(QLatin1String("//"))) {
                doCppStyleUncomment = false;
                break;
            }
        }
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            if (doCppStyleUncomment) {
                QString text = block.text();
                int i = 0;
                while (i < text.size() - 1) {
                    if (text.at(i) == QLatin1Char('/')
                        && text.at(i + 1) == QLatin1Char('/')) {
                        cursor.setPosition(block.position() + i);
                        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 2);
                        cursor.removeSelectedText();
                        break;
                    }
                    if (!text.at(i).isSpace())
                        break;
                    ++i;
                }
            } else {
                cursor.setPosition(block.position());
                cursor.insertText(QLatin1String("//"));
            }
        }
    }

    // adjust selection when commenting out
    if (hasSelection && !doCStyleUncomment && !doCppStyleUncomment) {
        cursor = textCursor();
        if (!doCStyleComment)
            start = startBlock.position(); // move the double slashes into the selection
        int lastSelPos = anchorIsStart ? cursor.position() : cursor.anchor();
        if (anchorIsStart) {
            cursor.setPosition(start);
            cursor.setPosition(lastSelPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(lastSelPos);
            cursor.setPosition(start, QTextCursor::KeepAnchor);
        }
        setTextCursor(cursor);
    }

    cursor.endEditBlock();
}

int CPPEditor::endOfNameAtPosition(int pos)
{
    if (pos == -1)
        pos = position();

    QChar chr = characterAt(pos);

    // Skip to the start of a name
    while (chr.isLetterOrNumber() || chr == QLatin1Char('_'))
        chr = characterAt(++pos);

    return pos;
}

CPPEditor::Link CPPEditor::linkToSymbol(CPlusPlus::Symbol *symbol)
{
    const QString fileName = QString::fromUtf8(symbol->fileName(),
                                               symbol->fileNameLength());
    unsigned line = symbol->line();
    unsigned column = symbol->column();

    if (column)
        --column;

    if (symbol->isGenerated())
        column = 0;

    return Link(fileName, line, column);
}

bool CPPEditor::openCppEditorAt(const Link &link)
{
    if (link.fileName.isEmpty())
        return false;

    if (baseTextDocument()->fileName() == link.fileName) {
        Core::EditorManager *editorManager = Core::EditorManager::instance();
        editorManager->addCurrentPositionToNavigationHistory();
        gotoLine(link.line, link.column);
        setFocus();
        return true;
    }

    return TextEditor::BaseTextEditor::openEditorAt(link.fileName,
                                                    link.line,
                                                    link.column,
                                                    Constants::C_CPPEDITOR);
}
