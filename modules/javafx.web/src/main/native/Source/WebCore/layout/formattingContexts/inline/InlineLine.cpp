/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "InlineLine.h"

#if ENABLE(LAYOUT_FORMATTING_CONTEXT)

#include "FontCascade.h"
#include "InlineFormattingContext.h"
#include "InlineSoftLineBreakItem.h"
#include "LayoutBoxGeometry.h"
#include "RuntimeEnabledFeatures.h"
#include "TextFlags.h"
#include "TextUtil.h"
#include <wtf/IsoMallocInlines.h>

namespace WebCore {
namespace Layout {

Line::Line(const InlineFormattingContext& inlineFormattingContext)
    : m_inlineFormattingContext(inlineFormattingContext)
    , m_trimmableTrailingContent(m_runs)
{
}

Line::~Line()
{
}

void Line::initialize()
{
    m_nonSpanningInlineLevelBoxCount = 0;
    m_contentLogicalWidth = { };
    m_runs.clear();
    m_trailingSoftHyphenWidth = { };
    m_trimmableTrailingContent.reset();
}

void Line::removeCollapsibleContent(InlineLayoutUnit extraHorizontalSpace)
{
    removeTrailingTrimmableContent();
    visuallyCollapsePreWrapOverflowContent(extraHorizontalSpace);
}

void Line::applyRunExpansion(InlineLayoutUnit extraHorizontalSpace)
{
    ASSERT(formattingContext().root().style().textAlign() == TextAlignMode::Justify);
    // Text is justified according to the method specified by the text-justify property,
    // in order to exactly fill the line box. Unless otherwise specified by text-align-last,
    // the last line before a forced break or the end of the block is start-aligned.
    if (m_runs.isEmpty() || m_runs.last().isLineBreak())
        return;
    // Anything to distribute?
    if (!extraHorizontalSpace)
        return;

    // Collect and distribute the expansion opportunities.
    size_t lineExpansionOpportunities = 0;
    Vector<size_t> runsExpansionOpportunities(m_runs.size());
    Vector<ExpansionBehavior> runsExpansionBehaviors(m_runs.size());
    auto lastRunIndexWithContent = std::optional<size_t> { };

    // Line start behaves as if we had an expansion here (i.e. fist runs should not start with allowing left expansion).
    auto runIsAfterExpansion = true;
    for (size_t runIndex = 0; runIndex < m_runs.size(); ++runIndex) {
        auto& run = m_runs[runIndex];
        auto& style = run.style();
        int expansionBehavior = DefaultExpansion;
        size_t expansionOpportunitiesInRun = 0;

        if (run.isText() && !TextUtil::shouldPreserveSpacesAndTabs(run.layoutBox())) {
            if (style.textCombine() == TextCombine::Horizontal)
                expansionBehavior = ForbidLeftExpansion | ForbidRightExpansion;
            else {
                expansionBehavior = (runIsAfterExpansion ? ForbidLeftExpansion : AllowLeftExpansion) | AllowRightExpansion;
                std::tie(expansionOpportunitiesInRun, runIsAfterExpansion) = FontCascade::expansionOpportunityCount(StringView(downcast<InlineTextBox>(run.layoutBox()).content()).substring(run.textContent()->start, run.textContent()->length), run.style().direction(), expansionBehavior);
            }
        } else if (run.isBox())
            runIsAfterExpansion = false;

        runsExpansionBehaviors[runIndex] = expansionBehavior;
        runsExpansionOpportunities[runIndex] = expansionOpportunitiesInRun;
        lineExpansionOpportunities += expansionOpportunitiesInRun;

        if (run.isText() || run.isBox())
            lastRunIndexWithContent = runIndex;
    }
    // Need to fix up the last run's trailing expansion.
    if (lastRunIndexWithContent && runsExpansionOpportunities[*lastRunIndexWithContent]) {
        // Turn off the trailing bits first and add the forbid trailing expansion.
        auto leadingExpansion = runsExpansionBehaviors[*lastRunIndexWithContent] & LeftExpansionMask;
        runsExpansionBehaviors[*lastRunIndexWithContent] = leadingExpansion | ForbidRightExpansion;
        if (runIsAfterExpansion) {
            // When the last run has an after expansion (e.g. CJK ideograph) we need to remove this trailing expansion opportunity.
            // Note that this is not about trailing collapsible whitespace as at this point we trimmed them all.
            ASSERT(lineExpansionOpportunities && runsExpansionOpportunities[*lastRunIndexWithContent]);
            --lineExpansionOpportunities;
            --runsExpansionOpportunities[*lastRunIndexWithContent];
        }
    }
    // Anything to distribute?
    if (!lineExpansionOpportunities)
        return;
    // Distribute the extra space.
    auto expansionToDistribute = extraHorizontalSpace / lineExpansionOpportunities;
    auto accumulatedExpansion = InlineLayoutUnit { };
    for (size_t runIndex = 0; runIndex < m_runs.size(); ++runIndex) {
        auto& run = m_runs[runIndex];
        // Expand and move runs by the accumulated expansion.
        run.moveHorizontally(accumulatedExpansion);
        auto computedExpansion = expansionToDistribute * runsExpansionOpportunities[runIndex];
        run.setExpansion({ runsExpansionBehaviors[runIndex], computedExpansion });
        run.shrinkHorizontally(-computedExpansion);
        accumulatedExpansion += computedExpansion;
    }
    // Content grows as runs expand.
    m_contentLogicalWidth += accumulatedExpansion;
}

void Line::removeTrailingTrimmableContent()
{
    if (m_trimmableTrailingContent.isEmpty() || m_runs.isEmpty())
        return;

    // Complex line layout quirk: keep the trailing whitespace around when it is followed by a line break, unless the content overflows the line.
    if (RuntimeEnabledFeatures::sharedFeatures().layoutFormattingContextIntegrationEnabled()) {
        auto isTextAlignRight = [&] {
            auto textAlign = formattingContext().root().style().textAlign();
            return textAlign == TextAlignMode::Right
                || textAlign == TextAlignMode::WebKitRight
                || textAlign == TextAlignMode::End;
            }();

        if (m_runs.last().isLineBreak() && !isTextAlignRight) {
            m_trimmableTrailingContent.reset();
            return;
        }
    }

    m_contentLogicalWidth -= m_trimmableTrailingContent.remove();
}

void Line::visuallyCollapsePreWrapOverflowContent(InlineLayoutUnit extraHorizontalSpace)
{
    ASSERT(m_trimmableTrailingContent.isEmpty());
    // If white-space is set to pre-wrap, the UA must
    // ...
    // It may also visually collapse the character advance widths of any that would otherwise overflow.
    auto overflowWidth = -extraHorizontalSpace;
    if (overflowWidth <= 0)
        return;
    // Let's just find the trailing pre-wrap whitespace content for now (e.g check if there are multiple trailing runs with
    // different set of white-space values and decide if the in-between pre-wrap content should be collapsed as well.)
    auto trimmedContentWidth = InlineLayoutUnit { };
    for (auto& run : WTF::makeReversedRange(m_runs)) {
        if (run.style().whiteSpace() != WhiteSpace::PreWrap) {
            // We are only interested in pre-wrap trailing content.
            break;
        }
        auto visuallyCollapsibleInlineItem = run.isInlineBoxStart() || run.isInlineBoxEnd() || run.hasTrailingWhitespace();
        if (!visuallyCollapsibleInlineItem)
            break;
        ASSERT(!run.hasCollapsibleTrailingWhitespace());
        auto trimmableWidth = InlineLayoutUnit { };
        if (run.isText()) {
            // FIXME: We should always collapse the run at a glyph boundary as the spec indicates: "collapse the character advance widths of any that would otherwise overflow"
            // and the trimmed width should be capped at std::min(run.trailingWhitespaceWidth(), overflowWidth) for text runs. Both FF and Chrome agree.
            trimmableWidth = run.visuallyCollapseTrailingWhitespace(overflowWidth);
        } else {
            trimmableWidth = run.logicalWidth();
            run.shrinkHorizontally(trimmableWidth);
        }
        trimmedContentWidth += trimmableWidth;
        overflowWidth -= trimmableWidth;
        if (overflowWidth <= 0)
            break;
    }
    m_contentLogicalWidth -= trimmedContentWidth;
}

void Line::append(const InlineItem& inlineItem, InlineLayoutUnit logicalWidth)
{
    if (inlineItem.isText())
        appendTextContent(downcast<InlineTextItem>(inlineItem), logicalWidth);
    else if (inlineItem.isLineBreak())
        appendLineBreak(inlineItem);
    else if (inlineItem.isWordBreakOpportunity())
        appendWordBreakOpportunity(inlineItem);
    else if (inlineItem.isInlineBoxStart())
        appendInlineBoxStart(inlineItem, logicalWidth);
    else if (inlineItem.isInlineBoxEnd())
        appendInlineBoxEnd(inlineItem, logicalWidth);
    else if (inlineItem.layoutBox().isReplacedBox())
        appendReplacedInlineLevelBox(inlineItem, logicalWidth);
    else if (inlineItem.isBox())
        appendNonReplacedInlineLevelBox(inlineItem, logicalWidth);
    else
        ASSERT_NOT_REACHED();
}

void Line::appendNonBreakableSpace(const InlineItem& inlineItem, InlineLayoutUnit logicalLeft, InlineLayoutUnit logicalWidth)
{
    m_runs.append({ inlineItem, logicalLeft, logicalWidth });
    // Do not let negative margin make the content shorter than it already is.
    auto runLogicalRight = logicalLeft + logicalWidth;
    m_contentLogicalWidth = std::max(m_contentLogicalWidth, runLogicalRight);
}

void Line::appendInlineBoxStart(const InlineItem& inlineItem, InlineLayoutUnit logicalWidth)
{
    // This is really just a placeholder to mark the start of the inline box <span>.
    ++m_nonSpanningInlineLevelBoxCount;
    appendNonBreakableSpace(inlineItem, contentLogicalRight(), logicalWidth);
}

void Line::appendInlineBoxEnd(const InlineItem& inlineItem, InlineLayoutUnit logicalWidth)
{
    // This is really just a placeholder to mark the end of the inline box </span>.
    auto removeTrailingLetterSpacing = [&] {
        if (!m_trimmableTrailingContent.isTrailingRunPartiallyTrimmable())
            return;
        m_contentLogicalWidth -= m_trimmableTrailingContent.removePartiallyTrimmableContent();
    };
    // Prevent trailing letter-spacing from spilling out of the inline box.
    // https://drafts.csswg.org/css-text-3/#letter-spacing-property See example 21.
    removeTrailingLetterSpacing();
    appendNonBreakableSpace(inlineItem, contentLogicalRight(), logicalWidth);
}

void Line::appendTextContent(const InlineTextItem& inlineTextItem, InlineLayoutUnit logicalWidth)
{
    auto& style = inlineTextItem.style();
    auto willCollapseCompletely = [&] {
        if (inlineTextItem.isEmptyContent())
            return true;
        if (!inlineTextItem.isWhitespace())
            return false;
        if (InlineTextItem::shouldPreserveSpacesAndTabs(inlineTextItem))
            return false;
        // Check if the last item is collapsed as well.
        for (auto& run : WTF::makeReversedRange(m_runs)) {
            if (run.isBox())
                return false;
            // https://drafts.csswg.org/css-text-3/#white-space-phase-1
            // Any collapsible space immediately following another collapsible space—even one outside the boundary of the inline containing that space,
            // provided both spaces are within the same inline formatting context—is collapsed to have zero advance width.
            // : "<span>  </span> " <- the trailing whitespace collapses completely.
            // Not that when the inline box has preserve whitespace style, "<span style="white-space: pre">  </span> " <- this whitespace stays around.
            if (run.isText())
                return run.hasCollapsibleTrailingWhitespace();
            ASSERT(run.isInlineBoxStart() || run.isInlineBoxEnd() || run.isWordBreakOpportunity());
        }
        // Leading whitespace.
        return true;
    };

    if (willCollapseCompletely())
        return;

    auto needsNewRun = [&] {
        if (m_runs.isEmpty())
            return true;
        auto& lastRun = m_runs.last();
        if (&lastRun.layoutBox() != &inlineTextItem.layoutBox())
            return true;
        if (!lastRun.isText())
            return true;
        if (lastRun.hasCollapsedTrailingWhitespace())
            return true;
        if (inlineTextItem.isWordSeparator() && style.fontCascade().wordSpacing())
            return true;
        return false;
    }();
    auto oldContentLogicalWidth = contentLogicalWidth();
    if (needsNewRun) {
        // Note, negative words spacing may cause glyph overlap.
        auto runLogicalLeft = contentLogicalRight() + (inlineTextItem.isWordSeparator() ? style.fontCascade().wordSpacing() : 0.0f);
        m_runs.append({ inlineTextItem, runLogicalLeft, logicalWidth });
        m_contentLogicalWidth = std::max(oldContentLogicalWidth, runLogicalLeft + logicalWidth);
    } else {
        m_runs.last().expand(inlineTextItem, logicalWidth);
        // Do not let negative letter spacing make the content shorter than it already is.
        m_contentLogicalWidth += std::max(0.0f, logicalWidth);
    }
    // Set the trailing trimmable content.
    if (inlineTextItem.isWhitespace() && !InlineTextItem::shouldPreserveSpacesAndTabs(inlineTextItem)) {
        m_trimmableTrailingContent.addFullyTrimmableContent(m_runs.size() - 1, contentLogicalWidth() - oldContentLogicalWidth);
        return;
    }
    // Any non-whitespace, no-trimmable content resets the existing trimmable.
    m_trimmableTrailingContent.reset();
    if (!formattingContext().layoutState().shouldIgnoreTrailingLetterSpacing() && !inlineTextItem.isWhitespace() && style.letterSpacing() > 0)
        m_trimmableTrailingContent.addPartiallyTrimmableContent(m_runs.size() - 1, style.letterSpacing());
    m_trailingSoftHyphenWidth = inlineTextItem.hasTrailingSoftHyphen() ? std::make_optional(style.fontCascade().width(TextRun { StringView { style.hyphenString() } })) : std::nullopt;
}

void Line::appendNonReplacedInlineLevelBox(const InlineItem& inlineItem, InlineLayoutUnit marginBoxLogicalWidth)
{
    m_trimmableTrailingContent.reset();
    m_trailingSoftHyphenWidth = { };
    m_contentLogicalWidth += marginBoxLogicalWidth;
    ++m_nonSpanningInlineLevelBoxCount;
    auto marginStart = formattingContext().geometryForBox(inlineItem.layoutBox()).marginStart();
    if (marginStart >= 0) {
        m_runs.append({ inlineItem, contentLogicalRight(), marginBoxLogicalWidth });
        return;
    }
    // Negative margin-start pulls the content to the logical left direction.
    // Negative margin also squeezes the margin box, we need to stretch it to make sure the subsequent content won't overlap.
    // e.g. <img style="width: 100px; margin-left: -100px;"> pulls the replaced box to -100px with the margin box width of 0px.
    // Instead we need to position it at -100px and size it to 100px so the subsequent content starts at 0px.
    m_runs.append({ inlineItem, contentLogicalRight() + marginStart, marginBoxLogicalWidth - marginStart });
}

void Line::appendReplacedInlineLevelBox(const InlineItem& inlineItem, InlineLayoutUnit marginBoxLogicalWidth)
{
    ASSERT(inlineItem.layoutBox().isReplacedBox());
    // FIXME: Surely replaced boxes behave differently.
    appendNonReplacedInlineLevelBox(inlineItem, marginBoxLogicalWidth);
}

void Line::appendLineBreak(const InlineItem& inlineItem)
{
    m_trailingSoftHyphenWidth = { };
    if (inlineItem.isHardLineBreak()) {
        ++m_nonSpanningInlineLevelBoxCount;
        return m_runs.append({ inlineItem, contentLogicalRight(), 0_lu });
    }
    // Soft line breaks (preserved new line characters) require inline text boxes for compatibility reasons.
    ASSERT(inlineItem.isSoftLineBreak());
    m_runs.append({ downcast<InlineSoftLineBreakItem>(inlineItem), contentLogicalRight() });
}

void Line::appendWordBreakOpportunity(const InlineItem& inlineItem)
{
    m_runs.append({ inlineItem, contentLogicalRight(), 0_lu });
}

void Line::addTrailingHyphen(InlineLayoutUnit hyphenLogicalWidth)
{
    for (auto& run : WTF::makeReversedRange(m_runs)) {
        if (!run.isText())
            continue;
        run.setNeedsHyphen(hyphenLogicalWidth);
        m_contentLogicalWidth += hyphenLogicalWidth;
        return;
    }
    ASSERT_NOT_REACHED();
}

const InlineFormattingContext& Line::formattingContext() const
{
    return m_inlineFormattingContext;
}

Line::TrimmableTrailingContent::TrimmableTrailingContent(RunList& runs)
    : m_runs(runs)
{
}

void Line::TrimmableTrailingContent::addFullyTrimmableContent(size_t runIndex, InlineLayoutUnit trimmableWidth)
{
    // Any subsequent trimmable whitespace should collapse to zero advanced width and ignored at ::appendTextContent().
    ASSERT(!m_hasFullyTrimmableContent);
    m_fullyTrimmableWidth = trimmableWidth;
    // Note that just because the trimmable width is 0 (font-size: 0px), it does not mean we don't have a trimmable trailing content.
    m_hasFullyTrimmableContent = true;
    m_firstTrimmableRunIndex = m_firstTrimmableRunIndex.value_or(runIndex);
}

void Line::TrimmableTrailingContent::addPartiallyTrimmableContent(size_t runIndex, InlineLayoutUnit trimmableWidth)
{
    // Do not add trimmable letter spacing after a fully trimmable whitespace.
    ASSERT(!m_firstTrimmableRunIndex);
    ASSERT(!m_hasFullyTrimmableContent);
    ASSERT(!m_partiallyTrimmableWidth);
    ASSERT(trimmableWidth);
    m_partiallyTrimmableWidth = trimmableWidth;
    m_firstTrimmableRunIndex = runIndex;
}

InlineLayoutUnit Line::TrimmableTrailingContent::remove()
{
    // Remove trimmable trailing content and move all the subsequent trailing runs.
    // <span> </span><span></span>
    // [trailing whitespace][inline box end][inline box start][inline box end]
    // Trim the whitespace run and move the trailing inline box runs to the logical left.
    ASSERT(!isEmpty());
    auto& trimmableRun = m_runs[*m_firstTrimmableRunIndex];
    ASSERT(trimmableRun.isText());

    if (m_hasFullyTrimmableContent)
        trimmableRun.removeTrailingWhitespace();
    if (m_partiallyTrimmableWidth)
        trimmableRun.removeTrailingLetterSpacing();

    auto trimmableWidth = width();
    // When the trimmable run is followed by some non-content runs, we need to adjust their horizontal positions.
    // e.g. <div>text is followed by trimmable content    <span> </span></div>
    // When the [text...] run is trimmed (trailing whitespace is removed), both "<span>" and "</span>" runs
    // need to be moved horizontally to catch up with the [text...] run. Note that the whitespace inside the <span> does
    // not produce a run since in ::appendText() we see it as a fully collapsible run.
    for (auto index = *m_firstTrimmableRunIndex + 1; index < m_runs.size(); ++index) {
        auto& run = m_runs[index];
        ASSERT(run.isWordBreakOpportunity() || run.isInlineBoxStart() || run.isInlineBoxEnd() || run.isLineBreak());
        run.moveHorizontally(-trimmableWidth);
    }
    if (!trimmableRun.textContent()->length) {
        // This trimmable run is fully collapsed now (e.g. <div><img>    <span></span></div>).
        // We don't need to keep it around anymore.
        m_runs.remove(*m_firstTrimmableRunIndex);
    }
    reset();
    return trimmableWidth;
}

InlineLayoutUnit Line::TrimmableTrailingContent::removePartiallyTrimmableContent()
{
    // Partially trimmable content is always gated by a fully trimmable content.
    // We can't just trim spacing in the middle.
    ASSERT(!m_fullyTrimmableWidth);
    return remove();
}

Line::Run::Run(const InlineItem& inlineItem, InlineLayoutUnit logicalLeft, InlineLayoutUnit logicalWidth)
    : m_type(inlineItem.type())
    , m_layoutBox(&inlineItem.layoutBox())
    , m_logicalLeft(logicalLeft)
    , m_logicalWidth(logicalWidth)
{
}

Line::Run::Run(const InlineSoftLineBreakItem& softLineBreakItem, InlineLayoutUnit logicalLeft)
    : m_type(softLineBreakItem.type())
    , m_layoutBox(&softLineBreakItem.layoutBox())
    , m_logicalLeft(logicalLeft)
    , m_textContent({ softLineBreakItem.position(), 1 })
{
}

Line::Run::Run(const InlineTextItem& inlineTextItem, InlineLayoutUnit logicalLeft, InlineLayoutUnit logicalWidth)
    : m_type(InlineItem::Type::Text)
    , m_layoutBox(&inlineTextItem.layoutBox())
    , m_logicalLeft(logicalLeft)
    , m_logicalWidth(logicalWidth)
    , m_trailingWhitespaceType(trailingWhitespaceType(inlineTextItem))
    , m_trailingWhitespaceWidth(m_trailingWhitespaceType != TrailingWhitespace::None ? logicalWidth : InlineLayoutUnit { })
    , m_textContent({ inlineTextItem.start(), m_trailingWhitespaceType == TrailingWhitespace::Collapsed ? 1 : inlineTextItem.length() })
{
}

void Line::Run::expand(const InlineTextItem& inlineTextItem, InlineLayoutUnit logicalWidth)
{
    ASSERT(!hasCollapsedTrailingWhitespace());
    ASSERT(isText() && inlineTextItem.isText());
    ASSERT(m_layoutBox == &inlineTextItem.layoutBox());

    m_logicalWidth += logicalWidth;
    m_trailingWhitespaceType = trailingWhitespaceType(inlineTextItem);

    if (m_trailingWhitespaceType == TrailingWhitespace::None) {
        m_trailingWhitespaceWidth = { };
        m_textContent->length += inlineTextItem.length();
        return;
    }
    m_trailingWhitespaceWidth += logicalWidth;
    m_textContent->length += m_trailingWhitespaceType == TrailingWhitespace::Collapsed ? 1 : inlineTextItem.length();
}

bool Line::Run::hasTrailingLetterSpacing() const
{
    return !hasTrailingWhitespace() && style().letterSpacing() > 0;
}

InlineLayoutUnit Line::Run::trailingLetterSpacing() const
{
    if (!hasTrailingLetterSpacing())
        return { };
    return InlineLayoutUnit { style().letterSpacing() };
}

void Line::Run::removeTrailingLetterSpacing()
{
    ASSERT(hasTrailingLetterSpacing());
    shrinkHorizontally(trailingLetterSpacing());
    ASSERT(logicalWidth() > 0 || (!logicalWidth() && style().letterSpacing() >= static_cast<float>(intMaxForLayoutUnit)));
}

void Line::Run::removeTrailingWhitespace()
{
    // According to https://www.w3.org/TR/css-text-3/#white-space-property matrix
    // Trimmable whitespace is always collapsible so the length of the trailing trimmable whitespace is always 1 (or non-existent).
    ASSERT(m_textContent->length);
    constexpr size_t trailingTrimmableContentLength = 1;
    m_textContent->length -= trailingTrimmableContentLength;
    visuallyCollapseTrailingWhitespace(m_trailingWhitespaceWidth);
}

InlineLayoutUnit Line::Run::visuallyCollapseTrailingWhitespace(InlineLayoutUnit tryCollapsingThisMuchSpace)
{
    ASSERT(hasTrailingWhitespace());
    // This is just a visual adjustment, the text length should remain the same.
    auto trimmedWidth = std::min(tryCollapsingThisMuchSpace, m_trailingWhitespaceWidth);
    shrinkHorizontally(trimmedWidth);
    m_trailingWhitespaceWidth -= trimmedWidth;
    if (!m_trailingWhitespaceWidth) {
        // We trimmed the trailing whitespace completely.
        m_trailingWhitespaceType = TrailingWhitespace::None;
    }
    return trimmedWidth;
}

}
}

#endif
