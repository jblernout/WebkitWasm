// BrowserInBrowser page clients (Phase 3 interactivity).
//
// BibChromeClient: the repaint signal. WebCore's non-composited invalidation
// funnels through ChromeClient::invalidateContentsAndRootView (and the two
// scroll-damage paths) — we fold every damage report into one dirty flag and
// bib_render() repaints the full frame when it's set. ChromeClient::
// scheduleRenderingUpdate() is intentionally NOT overridden: the default
// returns false, which makes RenderingUpdateScheduler fall back to a RunLoop
// timer — and bib_tick() cycles the RunLoop every rAF, so WebCore's own
// rendering updates (in-page rAF, intersection observers) get driven for free.
//
// BibEditorClient: typed text only reaches the DOM through
// EditorClient::handleKeyboardEvent (EmptyEditorClient is final AND inert —
// its should* gates return false, which BLOCKS caret placement and
// insertion). This is the EmptyEditorClient surface copied from
// EmptyClients.cpp with the gates flipped to true and the WinCairo
// WebPage::handleEditingKeyboardEvent logic inlined (minimal command map).

#pragma once

#include "Document.h"
#include "Editor.h"
#include "EditorClient.h"
#include "EmptyClients.h"
#include "EventNames.h"
#include "FrameDestructionObserverInlines.h" // inline FrameDestructionObserver::frame()
#include "KeyboardEvent.h"
#include "LocalFrame.h"
#include "Node.h"
#include "NodeDocument.h" // inline Node::document()
#include "PlatformKeyboardEvent.h"
#include "TextCheckerClient.h"
#include <wtf/text/WTFString.h>

namespace BIB {

// Set by BibChromeClient on any damage report; cleared by paintFrame().
inline bool g_frameDirty = true;

class BibChromeClient final : public WebCore::EmptyChromeClient {
public:
    BibChromeClient() = default;

private:
    void invalidateRootView(const WebCore::IntRect&) final { g_frameDirty = true; }
    void invalidateContentsAndRootView(const WebCore::IntRect&) final { g_frameDirty = true; }
    void invalidateContentsForSlowScroll(const WebCore::IntRect&) final { g_frameDirty = true; }
    void scroll(const WebCore::IntSize&, const WebCore::IntRect&, const WebCore::IntRect&) final { g_frameDirty = true; }
};

class BibEditorClient final : public WebCore::EditorClient {
public:
    BibEditorClient() = default;

private:
    // Editing gates: EmptyEditorClient returns false from all of these,
    // which disables editing wholesale. We allow everything.
    bool shouldDeleteRange(const std::optional<WebCore::SimpleRange>&) final { return true; }
    bool smartInsertDeleteEnabled() final { return false; }
    bool isSelectTrailingWhitespaceEnabled() const final { return false; }
    bool isContinuousSpellCheckingEnabled() final { return false; }
    void toggleContinuousSpellChecking() final { }
    bool isGrammarCheckingEnabled() final { return false; }
    void toggleGrammarChecking() final { }
    int spellCheckerDocumentTag() final { return -1; }

    bool shouldBeginEditing(const WebCore::SimpleRange&) final { return true; }
    bool shouldEndEditing(const WebCore::SimpleRange&) final { return true; }
    bool shouldInsertNode(WebCore::Node&, const std::optional<WebCore::SimpleRange>&, WebCore::EditorInsertAction) final { return true; }
    bool shouldInsertText(const String&, const std::optional<WebCore::SimpleRange>&, WebCore::EditorInsertAction) final { return true; }
    bool shouldChangeSelectedRange(const std::optional<WebCore::SimpleRange>&, const std::optional<WebCore::SimpleRange>&, WebCore::Affinity, bool) final { return true; }

    bool shouldApplyStyle(const WebCore::StyleProperties&, const std::optional<WebCore::SimpleRange>&) final { return true; }
    void didApplyStyle() final { }
    bool shouldMoveRangeAfterDelete(const WebCore::SimpleRange&, const WebCore::SimpleRange&) final { return true; }

    void didBeginEditing() final { }
    void respondToChangedContents() final { }
    void respondToChangedSelection(WebCore::LocalFrame*) final { }
    void updateEditorStateAfterLayoutIfEditabilityChanged() final { }
    void discardedComposition(const WebCore::Document&) final { }
    void canceledComposition() final { }
    void didUpdateComposition() final { }
    void didEndEditing() final { }
    void didEndUserTriggeredSelectionChanges() final { }
    void willWriteSelectionToPasteboard(const std::optional<WebCore::SimpleRange>&) final { }
    void didWriteSelectionToPasteboard() final { }
    void getClientPasteboardData(const std::optional<WebCore::SimpleRange>&, Vector<std::pair<String, RefPtr<WebCore::SharedBuffer>>>&) final { }
    void requestCandidatesForSelection(const WebCore::VisibleSelection&) final { }
    void handleAcceptedCandidateWithSoftSpaces(WebCore::TextCheckingResult) final { }

    void registerUndoStep(WebCore::UndoStep&) final { }
    void registerRedoStep(WebCore::UndoStep&) final { }
    void clearUndoRedoOperations() final { }

    WebCore::DOMPasteAccessResponse requestDOMPasteAccess(WebCore::DOMPasteAccessCategory, WebCore::FrameIdentifier, const String&) final { return WebCore::DOMPasteAccessResponse::DeniedForGesture; }

    bool canCopyCut(WebCore::LocalFrame*, bool defaultValue) const final { return defaultValue; }
    bool canPaste(WebCore::LocalFrame*, bool defaultValue) const final { return defaultValue; }
    bool canUndo() const final { return false; }
    bool canRedo() const final { return false; }

    void undo() final { }
    void redo() final { }

    void handleKeyboardEvent(WebCore::KeyboardEvent& event) final
    {
        if (handleEditingKeyboardEvent(event))
            event.setDefaultHandled();
    }
    void handleInputMethodKeydown(WebCore::KeyboardEvent&) final { }

    void textFieldDidBeginEditing(WebCore::Element&) final { }
    void textFieldDidEndEditing(WebCore::Element&) final { }
    void textDidChangeInTextField(WebCore::Element&) final { }
    bool doTextFieldCommandFromEvent(WebCore::Element&, WebCore::KeyboardEvent*) final { return false; }
    void textWillBeDeletedInTextField(WebCore::Element&) final { }
    void textDidChangeInTextArea(WebCore::Element&) final { }
    void overflowScrollPositionChanged() final { }
    void subFrameScrollPositionChanged() final { }

    bool performTwoStepDrop(WebCore::DocumentFragment&, const WebCore::SimpleRange&, bool) final { return false; }

    WebCore::TextCheckerClient* textChecker() final { return &m_textCheckerClient; }

    void updateSpellingUIWithGrammarString(const String&, const WebCore::GrammarDetail&) final { }
    void updateSpellingUIWithMisspelledWord(const String&) final { }
    void showSpellingUI(bool) final { }
    bool spellingUIIsShowing() final { return false; }

    void setInputMethodState(WebCore::Element*) final { }

    // Minimal interpretKeyEvent: just the commands the embedder needs for
    // basic field editing. Everything else falls through to text insertion
    // (Char events) or is ignored.
    static const char* commandForKeyDown(const WebCore::PlatformKeyboardEvent& event)
    {
        auto& key = event.key();
        if (key == "Backspace"_s)
            return "DeleteBackward";
        if (key == "Delete"_s)
            return "DeleteForward";
        if (key == "ArrowLeft"_s)
            return event.shiftKey() ? "MoveLeftAndModifySelection" : "MoveLeft";
        if (key == "ArrowRight"_s)
            return event.shiftKey() ? "MoveRightAndModifySelection" : "MoveRight";
        if (key == "ArrowUp"_s)
            return "MoveUp";
        if (key == "ArrowDown"_s)
            return "MoveDown";
        if (key == "Home"_s)
            return "MoveToBeginningOfLine";
        if (key == "End"_s)
            return "MoveToEndOfLine";
        return "";
    }

    static const char* commandForChar(const WebCore::PlatformKeyboardEvent& event)
    {
        if (event.text() == "\r"_s)
            return "InsertNewline";
        if (event.text() == "\t"_s)
            return "InsertTab";
        return "";
    }

    // WinCairo WebPage::handleEditingKeyboardEvent, condensed.
    static bool handleEditingKeyboardEvent(WebCore::KeyboardEvent& event)
    {
        RefPtr targetNode = dynamicDowncast<WebCore::Node>(event.target());
        if (!targetNode)
            return false;
        RefPtr frame = targetNode->document().frame();
        if (!frame)
            return false;

        auto* keyEvent = event.underlyingPlatformEvent();
        if (!keyEvent || keyEvent->isSystemKey())
            return false;
        if (event.type() != WebCore::eventNames().keydownEvent && event.type() != WebCore::eventNames().keypressEvent)
            return false;

        if (keyEvent->type() == WebCore::PlatformEvent::Type::RawKeyDown) {
            auto command = frame->editor().command(String::fromLatin1(commandForKeyDown(*keyEvent)));
            // Leave text-inserting commands to the keypress (Char) event so
            // WebCore can decide (e.g. Tab = focus move vs character).
            return !command.isTextInsertion() && command.execute(&event);
        }

        auto command = frame->editor().command(String::fromLatin1(commandForChar(*keyEvent)));
        if (command.execute(&event))
            return true;

        // Don't insert null or control characters.
        if (event.charCode() < ' ')
            return false;
        return frame->editor().insertText(keyEvent->text(), &event);
    }

    class BibTextCheckerClient final : public WebCore::TextCheckerClient {
        bool shouldEraseMarkersAfterChangeSelection(WebCore::TextCheckingType) const final { return true; }
        void ignoreWordInSpellDocument(const String&) final { }
        void learnWord(const String&) final { }
        void checkSpellingOfString(StringView, int*, int*) final { }
        void checkGrammarOfString(StringView, Vector<WebCore::GrammarDetail>&, int*, int*) final { }
#if USE(UNIFIED_TEXT_CHECKING)
        Vector<WebCore::TextCheckingResult> checkTextOfParagraph(StringView, OptionSet<WebCore::TextCheckingType>, const WebCore::VisibleSelection&) final { return { }; }
#endif
        void getGuessesForWord(const String&, const String&, const WebCore::VisibleSelection&, Vector<String>&) final { }
        void requestCheckingOfString(WebCore::TextCheckingRequest&, const WebCore::VisibleSelection&) final { }
        void requestExtendedCheckingOfString(WebCore::TextCheckingRequest&, const WebCore::VisibleSelection&) final { }
    };

    BibTextCheckerClient m_textCheckerClient;
};

} // namespace BIB
