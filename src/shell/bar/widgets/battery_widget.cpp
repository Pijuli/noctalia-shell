#include "shell/bar/widgets/battery_widget.h"

#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <utility>

namespace {

  constexpr float kGraphicBodyWidth = 22.0f;
  constexpr float kGraphicBodyHeight = 14.0f;
  constexpr float kGraphicTerminalWidth = 2.5f;
  constexpr float kGraphicTerminalHeight = 7.0f;
  constexpr float kGraphicCornerRadius = 3.0f;

  ColorSpec withOpacity(ColorSpec color, float opacity) {
    color.alpha *= opacity;
    return color;
  }

  const char* batteryGlyphName(double percentage, BatteryState state) {
    if (state == BatteryState::Charging) {
      return "battery-charging";
    }
    if (state == BatteryState::FullyCharged || state == BatteryState::PendingCharge) {
      return "battery-plugged";
    }
    if (state == BatteryState::Unknown && percentage <= 0.0) {
      return "battery-exclamation";
    }
    if (percentage >= 85.0) {
      return "battery-4";
    }
    if (percentage >= 55.0) {
      return "battery-3";
    }
    if (percentage >= 30.0) {
      return "battery-2";
    }
    if (percentage >= 10.0) {
      return "battery-1";
    }
    return "battery-0";
  }

  const char* batteryStateGlyph(BatteryState state) {
    if (state == BatteryState::Charging) {
      return "bolt-filled";
    }
    if (state == BatteryState::FullyCharged || state == BatteryState::PendingCharge) {
      return "plug-filled";
    }
    return nullptr;
  }

} // namespace

BatteryWidget::BatteryWidget(
    UPowerService* upower, std::string deviceSelector, int warningThreshold, ColorSpec warningColor,
    BatteryDisplayMode displayMode, bool showLabel, bool hideWhenPlugged, bool hideWhenFull
)
    : m_upower(upower), m_deviceSelector(std::move(deviceSelector)), m_warningThreshold(warningThreshold),
      m_warningColor(warningColor), m_displayMode(displayMode), m_showLabel(showLabel),
      m_hideWhenPlugged(hideWhenPlugged), m_hideWhenFull(hideWhenFull) {}

void BatteryWidget::create() {
  auto container = std::make_unique<InputArea>();
  setRoot(std::move(container));

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    createGraphicMode();
  } else {
    createGlyphMode();
  }
}

void BatteryWidget::createGraphicMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::box({
          .out = &m_bodyBg,
          .fill = withOpacity(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.25f),
      })
  );

  container->addChild(
      ui::box({
          .out = &m_fillRect,
      })
  );

  container->addChild(
      ui::box({
          .out = &m_terminalNub,
          .fill = withOpacity(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.25f),
      })
  );

  // The number is drawn twice, each copy clipped to one side of the fill boundary so the digits
  // contrast whatever is behind them (see updateFillGeometry).
  auto makeInlineLabel = [&](Node*& clipOut, Label*& labelOut) {
    auto clip = std::make_unique<Node>();
    clip->setClipChildren(true);
    clipOut = container->addChild(std::move(clip));

    auto label = std::make_unique<Label>();
    label->setFontFamily(labelFontFamily());
    label->setFontWeight(labelFontWeight());
    // InkCentered, not the default cap-band centering: fonts that report no cap height fall back
    // to top-alignment, which left the number riding high and clipped against the body's top.
    label->setBaselineMode(LabelBaselineMode::InkCentered);
    labelOut = static_cast<Label*>(clipOut->addChild(std::move(label)));
  };
  makeInlineLabel(m_inlineFillClip, m_labelOnFill);
  makeInlineLabel(m_inlineEmptyClip, m_labelOnEmpty);

  container->addChild(
      ui::glyph({
          .out = &m_overlayGlyph,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .visible = false,
      })
  );
}

void BatteryWidget::createGlyphMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "battery-4",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  container->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontFamily = labelFontFamily(),
          .fontWeight = labelFontWeight(),
          .visible = m_showLabel,
      })
  );
}

void BatteryWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    layoutGraphicMode(renderer);
  } else {
    layoutGlyphMode(renderer, containerWidth, containerHeight);
  }
}

void BatteryWidget::layoutGraphicMode(Renderer& renderer) {
  auto* rootNode = root();
  if (m_bodyBg == nullptr || m_fillRect == nullptr || m_terminalNub == nullptr || rootNode == nullptr) {
    return;
  }

  const float scale = (Style::fontSizeBody / 14.0f) * m_contentScale;
  float bodyW = std::round(kGraphicBodyWidth * scale);
  float bodyH = std::round(kGraphicBodyHeight * scale);
  const float termW = std::round(kGraphicTerminalWidth * scale);
  const float termH = std::round(kGraphicTerminalHeight * scale);
  const float cornerR = std::round(kGraphicCornerRadius * scale);
  const float labelGap = Style::spaceXs * m_contentScale;
  const bool showLabel = m_showLabel && m_labelOnFill != nullptr;
  const bool showStateGlyph = m_overlayGlyph != nullptr && m_overlayGlyph->visible();
  // The state icon sits beside the battery when the number is shown, else centered in the body.
  const bool showStateGlyphOutside = showStateGlyph && showLabel;
  const bool showStateGlyphInside = showStateGlyph && !showLabel;
  if (showStateGlyph) {
    m_overlayGlyph->setGlyphSize(Style::fontSizeCaption * m_contentScale);
    m_overlayGlyph->measure(renderer);
  }

  const float labelW = showLabel ? m_inlineLabelW : 0.0f;

  // Widen only the digit-width axis to fit the number; the cross axis keeps the natural battery
  // height (growing it just makes the battery look too tall).
  if (showLabel) {
    const float padX = std::round(Style::spaceXs * m_contentScale);
    if (m_isVertical) {
      bodyH = std::max(bodyH, labelW + 2.0f * padX);
    } else {
      bodyW = std::max(bodyW, labelW + 2.0f * padX);
    }
  }

  if (m_isVertical) {
    const float graphicW = bodyH;
    const float graphicH = bodyW + termW;
    const float stateW = showStateGlyphOutside ? m_overlayGlyph->width() : 0.0f;
    const float stateH = showStateGlyphOutside ? m_overlayGlyph->height() : 0.0f;
    const float rootW = std::max(graphicW, stateW);
    const float bodyX = std::round((rootW - graphicW) * 0.5f);
    const float bodyY = termW;

    m_bodyBg->setRadius(cornerR);
    m_bodyBg->setPosition(bodyX, bodyY);
    m_bodyBg->setSize(bodyH, bodyW);

    m_terminalNub->setRadius(cornerR * 0.5f);
    m_terminalNub->setPosition(bodyX + std::round((bodyH - termH) * 0.5f), 0.0f);
    m_terminalNub->setSize(termH, termW);

    m_fillRect->setRadius(cornerR);
    updateFillGeometry();

    if (showStateGlyphOutside) {
      m_overlayGlyph->setPosition(std::round((rootW - stateW) * 0.5f), graphicH + labelGap);
    } else if (showStateGlyphInside) {
      m_overlayGlyph->setPosition(
          bodyX + std::round((bodyH - m_overlayGlyph->width()) * 0.5f),
          bodyY + std::round((bodyW - m_overlayGlyph->height()) * 0.5f)
      );
    }

    rootNode->setSize(rootW, graphicH + (showStateGlyphOutside ? labelGap + stateH : 0.0f));
  } else {
    const float graphicW = bodyW + termW;
    const float graphicH = bodyH;
    const float stateW = showStateGlyphOutside ? m_overlayGlyph->width() : 0.0f;
    const float stateH = showStateGlyphOutside ? m_overlayGlyph->height() : 0.0f;
    const float rootH = std::max(graphicH, stateH);
    const float bodyY = std::round((rootH - bodyH) * 0.5f);

    m_bodyBg->setRadius(cornerR);
    m_bodyBg->setPosition(0.0f, bodyY);
    m_bodyBg->setSize(bodyW, bodyH);

    m_terminalNub->setRadius(cornerR * 0.5f);
    m_terminalNub->setPosition(bodyW, bodyY + std::round((bodyH - termH) * 0.5f));
    m_terminalNub->setSize(termW, termH);

    m_fillRect->setRadius(cornerR);
    updateFillGeometry();

    if (showStateGlyphOutside) {
      m_overlayGlyph->setPosition(graphicW + labelGap, std::round((rootH - stateH) * 0.5f));
    } else if (showStateGlyphInside) {
      m_overlayGlyph->setPosition(
          std::round((bodyW - m_overlayGlyph->width()) * 0.5f),
          bodyY + std::round((bodyH - m_overlayGlyph->height()) * 0.5f)
      );
    }

    rootNode->setSize(graphicW + (showStateGlyphOutside ? labelGap + stateW : 0.0f), rootH);
  }
}

void BatteryWidget::layoutGlyphMode(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  m_glyph->measure(renderer);

  if (m_label != nullptr && m_showLabel) {
    m_label->measure(renderer);

    if (m_isVertical) {
      const float w = std::max(m_glyph->width(), m_label->width());
      m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5f), 0.0f);
      m_label->setPosition(std::round((w - m_label->width()) * 0.5f), m_glyph->height());
      rootNode->setSize(w, m_glyph->height() + m_label->height());
    } else {
      const float h = std::max(m_glyph->height(), m_label->height());
      m_glyph->setPosition(0.0f, std::round((h - m_glyph->height()) * 0.5f));
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
      rootNode->setSize(m_label->x() + m_label->width(), h);
    }
  } else {
    rootNode->setSize(m_glyph->width(), m_glyph->height());
  }
}

void BatteryWidget::updateFillGeometry() {
  if (m_fillRect == nullptr || m_bodyBg == nullptr) {
    return;
  }

  const float fraction = std::clamp(m_animatedPct / 100.0f, 0.0f, 1.0f);

  if (m_isVertical) {
    const float bodyW = m_bodyBg->width();
    const float bodyH = m_bodyBg->height();
    const float fillH = bodyH * fraction;
    m_fillRect->setPosition(m_bodyBg->x(), m_bodyBg->y() + bodyH - fillH);
    m_fillRect->setSize(bodyW, fillH);
  } else {
    const float bodyW = m_bodyBg->width();
    const float bodyH = m_bodyBg->height();
    const float fillW = bodyW * fraction;
    m_fillRect->setPosition(m_bodyBg->x(), m_bodyBg->y());
    m_fillRect->setSize(fillW, bodyH);
  }

  // Inline two-tone number: clip one copy to the filled rect and the other to the empty remainder,
  // both centered on the body. The clip frames track the fill, so the color split animates with it.
  if (m_inlineFillClip != nullptr
      && m_inlineEmptyClip != nullptr
      && m_labelOnFill != nullptr
      && m_labelOnEmpty != nullptr) {
    const float bx = m_bodyBg->x();
    const float by = m_bodyBg->y();
    const float bw = m_bodyBg->width();
    const float bh = m_bodyBg->height();
    const float cx = bx + std::round((bw - m_inlineLabelW) * 0.5f);
    const float cy = by + std::round((bh - m_inlineLabelH) * 0.5f);

    const float fx = m_fillRect->x();
    const float fy = m_fillRect->y();
    const float fw = m_fillRect->width();
    const float fh = m_fillRect->height();

    m_inlineFillClip->setPosition(fx, fy);
    m_inlineFillClip->setFrameSize(fw, fh);
    m_labelOnFill->setPosition(cx - fx, cy - fy);

    // Empty side = body minus the fill (to its right horizontally, above it vertically).
    float ex = bx;
    float ey = by;
    float ew = bw;
    float eh = bh;
    if (m_isVertical) {
      eh = bh - fh;
    } else {
      ex = bx + fw;
      ew = bw - fw;
    }
    m_inlineEmptyClip->setPosition(ex, ey);
    m_inlineEmptyClip->setFrameSize(ew, eh);
    m_labelOnEmpty->setPosition(cx - ex, cy - ey);
  }
}

void BatteryWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void BatteryWidget::onFrameTick(float /*deltaMs*/) { requestRedraw(); }

bool BatteryWidget::needsFrameTick() const { return m_displayMode == BatteryDisplayMode::Graphic && m_fillAnim != 0; }

void BatteryWidget::syncState(Renderer& renderer) {
  if (m_upower == nullptr) {
    return;
  }

  const auto s = m_upower->stateForDevice(m_deviceSelector);

  const auto now = std::chrono::steady_clock::now();
  const bool forceTimeRefresh = (m_lastTooltipRefreshTime == std::chrono::steady_clock::time_point{})
      || (now - m_lastTooltipRefreshTime >= std::chrono::seconds(15));

  if (s.percentage == m_lastPct
      && s.state == m_lastState
      && s.isPresent == m_lastPresent
      && s.energyRate == m_lastEnergyRate
      && s.timeToEmpty == m_lastTimeToEmpty
      && m_isVertical == m_lastVertical
      && !forceTimeRefresh) {
    return;
  }

  m_lastPct = s.percentage;
  m_lastState = s.state;
  m_lastPresent = s.isPresent;
  m_lastEnergyRate = s.energyRate;
  m_lastTimeToEmpty = s.timeToEmpty;
  m_lastVertical = m_isVertical;
  m_lastTooltipRefreshTime = now;

  const bool isPluggedIn = s.state == BatteryState::Charging
      || s.state == BatteryState::FullyCharged
      || s.state == BatteryState::PendingCharge;

  const bool showWidget = s.isPresent
      && !(m_hideWhenPlugged && isPluggedIn)
      && !(m_hideWhenFull && (s.state == BatteryState::FullyCharged || s.state == BatteryState::PendingCharge));

  auto* rootNode = root();
  if (rootNode != nullptr) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }

  if (!showWidget) {
    return;
  }

  const int pct = static_cast<int>(std::round(s.percentage));
  const bool isWarning = m_warningThreshold > 0 && pct <= m_warningThreshold && !isPluggedIn;

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    const ColorSpec normalFgColor = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_fillRect != nullptr) {
      m_fillRect->setFill(fgColor);
    }
    if (m_bodyBg != nullptr) {
      m_bodyBg->setFill(withOpacity(fgColor, 0.25f));
    }

    if (m_terminalNub != nullptr) {
      m_terminalNub->setFill(isWarning ? m_warningColor : withOpacity(normalFgColor, 0.25f));
    }

    // Animate fill percentage
    const auto newPct = static_cast<float>(s.percentage);
    if (m_animations != nullptr && std::abs(m_animatedPct - newPct) > 0.5f) {
      m_animations->cancel(m_fillAnim);
      m_fillAnim = m_animations->animate(
          m_animatedPct, newPct, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
          [this](float v) {
            m_animatedPct = v;
            updateFillGeometry();
            requestRedraw();
          },
          [this]() { m_fillAnim = 0; }, this
      );
      requestFrameTick();
    } else {
      m_animatedPct = newPct;
      updateFillGeometry();
    }

    // Both copies share text and size, differing only in color: the copy over the bright fill
    // uses the contrasting Surface color, the copy over the empty body uses the foreground color.
    const std::string text = std::to_string(pct);
    const float fontSize = Style::fontSizeCaption * m_contentScale;
    auto applyInline = [&](Label* label, const ColorSpec& color) {
      if (label == nullptr) {
        return;
      }
      label->setText(text);
      label->setFontSize(fontSize);
      label->setColor(color);
      label->setVisible(m_showLabel);
      label->measure(renderer);
    };
    applyInline(m_labelOnFill, colorSpecFromRole(ColorRole::Surface));
    applyInline(m_labelOnEmpty, fgColor);
    if (m_labelOnFill != nullptr) {
      m_inlineLabelW = m_labelOnFill->width();
      m_inlineLabelH = m_labelOnFill->height();
    }

    // Overlay glyph — state icon
    const char* stateGlyph = batteryStateGlyph(s.state);
    if (m_overlayGlyph != nullptr) {
      if (stateGlyph != nullptr) {
        m_overlayGlyph->setGlyph(stateGlyph);
        m_overlayGlyph->setColor(fgColor);
        m_overlayGlyph->measure(renderer);
      }
    }

    if (m_overlayGlyph != nullptr) {
      m_overlayGlyph->setVisible(stateGlyph != nullptr);
    }
  } else {
    const ColorSpec normalFgColor = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_glyph != nullptr) {
      m_glyph->setGlyph(batteryGlyphName(s.percentage, s.state));
      m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      m_glyph->setColor(fgColor);
      m_glyph->measure(renderer);
    }

    if (m_label != nullptr && m_showLabel) {
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
      m_label->setText(m_isVertical ? std::to_string(pct) : std::to_string(pct) + "%");
      m_label->setColor(fgColor);
      m_label->measure(renderer);
    }
  }

  // Tooltip (both modes)
  if (rootNode != nullptr) {
    auto devices = m_upower->batteryDevices();
    auto laptopEnd =
        std::ranges::stable_partition(devices, [](const UPowerDeviceInfo& d) { return d.isLaptopBattery(); }).begin();
    int laptopBatteryCount = static_cast<int>(laptopEnd - devices.begin());

    std::vector<TooltipRow> rows;
    int laptopBatteryIndex = 0;
    for (const auto& dev : devices) {
      std::string name;
      if (dev.isLaptopBattery()) {
        name = (laptopBatteryCount > 1) ? ("Battery " + std::to_string(++laptopBatteryIndex)) : "Battery";
      } else {
        name = !dev.model.empty() ? dev.model : (!dev.nativePath.empty() ? dev.nativePath : "Unknown Device");
      }
      int dp = static_cast<int>(std::round(dev.state.percentage));
      rows.push_back({std::move(name), std::to_string(dp) + "%"});

      if (dev.isLaptopBattery()) {
        rows.push_back({i18n::tr("power.battery.tooltip.status"), batteryStateLabel(dev.state.state)});

        if (dev.state.timeToEmpty > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToEmpty));
          rows.push_back({i18n::tr("power.battery.tooltip.time-left"), std::move(dur)});
        } else if (dev.state.timeToFull > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToFull));
          rows.push_back({i18n::tr("power.battery.tooltip.time-to-full"), std::move(dur)});
        }

        if (dev.state.energyRate > 0.0) {
          std::ostringstream oss;
          oss << std::fixed;
          oss.precision(1);
          oss << dev.state.energyRate << " W";
          rows.push_back({i18n::tr("power.battery.tooltip.rate"), oss.str()});
        }

        if (dev.energyFullDesign > 0.0) {
          int health = static_cast<int>(std::round(dev.energyFull / dev.energyFullDesign * 100.0));
          rows.push_back({i18n::tr("power.battery.tooltip.health"), std::to_string(health) + "%"});
        }
      }
    }
    if (!rows.empty()) {
      static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));
    } else {
      static_cast<InputArea*>(rootNode)->clearTooltip();
    }
  }

  requestRedraw();
}
