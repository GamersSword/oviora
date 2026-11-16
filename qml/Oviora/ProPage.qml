// The stream menu's picture settings: the settings inspector of the old professional
// page, embedded in StreamMenu.qml. Tabs: 画质 / 补帧 / 色彩 / 声音 / 显示.
//
// The chain order is fixed (SR → NR layers → protect → HDR → 补帧) and is NOT
// reorderable here: dragging breaks the pipeline for beginners.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "EffectSupport.js" as Support

VPage {
    id: root
    readonly property bool hdrMeterObserved: visible && tab === "display" && !!(displayLoader.item && displayLoader.item.hdrMeterObserved)
    onHdrMeterObservedChanged: oviora.observeHdrCoverage(hdrMeterObserved)
    // The stream menu's copy is destroyed when the menu closes, without a change signal.
    Component.onDestruction: if (hdrMeterObserved && typeof oviora !== "undefined" && oviora) oviora.observeHdrCoverage(false)
    // Leaving the page disarms region drawing. The stream menu's copy is the exception: the menu
    // closes on purpose while the region is drawn on the picture, and opens again afterwards.
    onVisibleChanged: if (!visible && !embedded && oviora.protectionDrawShape.length > 0) oviora.beginProtectionDraw("")
    // After a drawing: the region card open and in view (the menu was rebuilt meanwhile)
    function revealMask(allEffects) {
        qualityLoader.active = true
        const q = qualityLoader.item
        if (!q) return
        const card = allEffects ? q.exclusionCard : q.protectionCard
        if (!card) return
        card.open = true
        revealTimer.card = card
        revealTimer.restart()
    }
    // Test runs: the RTX Video HDR card in view
    function revealVideoHdr() {
        qualityLoader.active = true
        if (!qualityLoader.item) return
        revealTimer.card = qualityLoader.item.videoHdrCard
        revealTimer.restart()
    }
    // Scrolled once the tab's cards are laid out and the card has opened
    Timer {
        id: revealTimer
        property Item card: null
        interval: 450
        onTriggered: {
            if (!card) return
            const y = card.mapToItem(body, 0, 0).y
            inspFlick.contentY = Math.max(0, Math.min(y - 12, inspFlick.contentHeight - inspFlick.height))
            oviora.logUi("ui-protection", "card shown at y=" + Math.round(y))
        }
    }

    // M31: switching tabs redraws the tab's cards one after another
    // (pages-pro.js render(true): 460 ms, i*30 ms, y 10).
    Component { id: staggerComp; VRise.Stagger {} }
    function stagger(col, step, span, dy) {
        if (Theme.reduced || !col) return
        let i = 0
        for (let k = 0; k < col.children.length; ++k) {
            const c = col.children[k]
            if (!c.visible || c.motionDy === undefined) continue
            staggerComp.createObject(root, { target: c, delay: i * step, span: span, dy: dy }).start()
            ++i
        }
    }
    onTabChanged: {
        if (tab === "quality") qualityLoader.active = true
        else if (tab === "fg") fgLoader.active = true
        else if (tab === "color") colourLoader.active = true
        else if (tab === "audio") audioLoader.active = true
        else if (tab === "display") displayLoader.active = true
        Qt.callLater(() => stagger(tab === "quality" ? qualityLoader.item
                                   : tab === "fg" ? fgLoader.item
                                   : tab === "color" ? (colourLoader.item ? colourLoader.item.sectionColumn : null)
                                   : tab === "audio" ? audioLoader.item : displayLoader.item, 30, 460, 10))
    }

    // M30: the NR layer card that was just added pops in; the number is reset
    // shortly after so a later rebuild of the list does not pop it again.
    property int popLayer: -1
    Timer { id: popReset; interval: 900; onTriggered: root.popLayer = -1 }
    function markPop(number) { popLayer = number; popReset.restart() }

    // Design's st.tab: quality / fg / color / audio / display.
    property string tab: "quality"
    property int selectedEffect: -1
    onSelectedEffectChanged: {
        oviora.selectedNrLayer = selectedEffect
        oviora.selectedColourLayer = selectedEffect
    }

    // The NR layer card wraps the shared editor (NrLayerEditor.qml).
    component NrLayerCard: VAccordion {
        id: card
        required property var layerData
        property int layerNumber: 1
        property int layerCount: 1
        property bool nrFirst: false
        signal edited(int nodeIndex, string key, double amount)
        signal enabledEdited(int nodeIndex, bool enabled)
        signal duplicateRequested(int nodeIndex)
        signal removeRequested(int nodeIndex)
        signal orderEdited(bool enabled)
        objectName: "nr-card-" + layerNumber
        Layout.fillWidth: true
        glyph: "wand"; hue: Theme.accent
        compactHeader: true
        popIn: layerNumber === root.popLayer
        title: qsTr("NR 层 ") + layerNumber
        // .lnum: the layer number where a stage card has its icon.
        headerLeadingActions: Rectangle {
            implicitWidth: 22; implicitHeight: 22; radius: 7
            color: Qt.rgba(1, 138 / 255, 61 / 255, 0.16)
            Text {
                textFormat: Text.PlainText
                anchors.centerIn: parent
                text: card.layerNumber
                color: Theme.accent
                font.family: Theme.fontMono; font.pixelSize: 11
                font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
            }
        }
        headerActions: VButton {
            objectName: "nr-card-more-" + card.layerNumber
            icon: true; ghost: true; iconName: "dup"
            implicitWidth: 26; implicitHeight: 26
            tip: qsTr("复制 · 恢复默认 · 删除")
            onClicked: { nrLayerMenu.layer = card; nrLayerMenu.openAt(this, "down") }
        }
        // This layer's own GPU time (list mode keys a node by its index + 2).
        readonly property var timing: (oviora.nodeTimings && oviora.nodeTimings[String(layerData.index + 2)]) || ({})
        readonly property string timingText: timing.state === "measured" ? timing.ms.toFixed(2) + " ms"
                                           : timing.state === "pending" ? qsTr("等待样本")
                                           : timing.state === "failed" ? qsTr("启用失败") : ""
        summary: nrEditor.sizeLabel + qsTr(" · 强度 ") + layerData.intensity.toFixed(2)
                 + (layerData.enabled ? (timingText.length > 0 ? " · " + timingText : "") : qsTr(" · 已旁路"))
        on: layerData.enabled
        onToggled: on => enabledEdited(layerData.index, on)
        NrLayerEditor {
            id: nrEditor
            Layout.fillWidth: true
            layerData: card.layerData
            layerCount: card.layerCount
            nrFirst: card.nrFirst
            onEdited: (i, k, v) => card.edited(i, k, v)
            onDuplicateRequested: i => card.duplicateRequested(i)
            onRemoveRequested: i => card.removeRequested(i)
            onOrderEdited: e => card.orderEdited(e)
        }
    }

    // .addlayer: the dashed button at the bottom of a layered stage (NR).
    component AddLayerButton: ProAddLayerButton {}

    Rectangle {
        id: insp
        objectName: "pro-inspector"
        anchors.fill: parent
        anchors.margins: 16
        radius: Theme.rCard
        color: Theme.card
        border.width: 1
        border.color: Theme.stroke
        clip: true
        // A click on the panel's own background is not a click on the picture.
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: root.tab === "color" ? insp.border.width : 0
            anchors.rightMargin: root.tab === "color" ? insp.border.width : 0
            spacing: 0

            // .insp-tabs: five tabs, equal width, 30px, radius 9.
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 8
                spacing: 2
                Repeater {
                    model: [
                        { id: "quality", label: qsTr("画质") },
                        { id: "fg", label: qsTr("补帧") },
                        { id: "color", label: qsTr("色彩") },
                        { id: "audio", label: qsTr("声音") },
                        { id: "display", label: qsTr("显示") }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 30
                        radius: 9
                        color: root.tab === modelData.id ? Theme.card3
                             : tabHover.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.d(200) } }
                        Text {
                            textFormat: Text.PlainText
                            anchors.centerIn: parent
                            text: modelData.label
                            color: root.tab === modelData.id ? Theme.t1 : Theme.t2
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsBody
                            font.weight: Font.Medium
                        }
                        HoverHandler { id: tabHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.tab = modelData.id }
                    }
                }
            }


            // Body. Each tab shows the controls the engine actually has; where the
            // engine has no control yet, the panel says so instead of offering one
            // that would do nothing.
            Flickable {
                id: inspFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 8
                contentHeight: body.implicitHeight
                clip: true
                ScrollBar.vertical: VScrollBar {
                    id: inspectorScroll
                    objectName: "pro-inspector-scrollbar"
                    wide: root.tab === "color"
                }

                ColumnLayout {
                    id: body
                    // CSS thin scrollbars consume 10px on the reference host;
                    // reserve their gutter instead of drawing over the colour card.
                    width: parent.width - 20 - (root.tab === "color" && inspectorScroll.size < 1 ? inspectorScroll.implicitWidth : 0)
                    x: 10
                    spacing: 6

                    // --- 画质 -------------------------------------------------
                    Loader {
                        id: qualityLoader
                        Layout.fillWidth: true
                        visible: root.tab === "quality"
                        active: false
                        Component.onCompleted: if (root.tab === "quality") active = true
                        sourceComponent: Component {
                            ColumnLayout {
                                id: qualityCol
                                spacing: 6
                                property alias exclusionCard: exclusionCard
                                property alias protectionCard: protectionCard
                                property alias videoHdrCard: videoHdrCard

                        // The design builds the inspector from accordion cards, not
                        // flat rows: an icon, a title, a live summary line, a switch
                        // and a chevron, with nested groups inside.
                        // Spatial anti-aliasing (#53) runs at source size right after the
                        // input conversion, in front of SR, so jaggies are never enlarged.
                        VAccordion {
                            objectName: "list-anti-alias"
                            Layout.fillWidth: true
                            glyph: "type"
                            hue: "#7FD4FF"
                            title: qsTr("抗锯齿")
                            summary: oviora.antiAliasEnabled ? oviora.antiAliasParams.summary : qsTr("已关闭")
                            on: oviora.antiAliasEnabled
                            onToggled: on => oviora.antiAliasEnabled = on
                            AntiAliasEditor {
                                Layout.fillWidth: true
                                namePrefix: "list-aa"
                                params: oviora.antiAliasParams
                                onEdited: (key, amount) => oviora.setAntiAliasParameter(key, amount)
                            }
                        }

                        VAccordion {
                            objectName: "list-sr"
                            Layout.fillWidth: true
                            glyph: "sparkles"
                            hue: "#4F7BFF"
                            title: qsTr("超分辨率")
                            switchAvailable: Support.available(oviora, "sr")
                            summary: !switchAvailable ? Support.reason(oviora, "sr") : (oviora.srEnabled ? (oviora.srBackendChoices?.find(o => o.id === oviora.srAlgorithmOf(oviora.videoSrQuality))?.label || qsTr("RTX 视频超分")) : qsTr("已关闭"))
                                     + " · " + oviora.srTargetLabel
                                     + (supersampleShown ? "→" + (oviora.srOutput === 1 ? qsTr("片源尺寸") : ["", "", "2K", "4K", "5K", "6K", "7K"][oviora.srOutput]) : "")
                                     + (dualShown ? "→" + (oviora.srPrescale >= 150 ? (oviora.srPrescale - 100) + "%" : ["", "720p", "900p", "1080p", "1440p"][oviora.srPrescale]) + "→" + ["", "2K", "4K", "8K", "5K", "6K", "7K"][oviora.dualSrTargetIndex] : "")
                                     + (rtxVideoSr ? " · " + oviora.srQualityLabel(oviora.videoSrQuality, false) + (dualShown ? " / " + secondLabel : "")
                                        : dualShown ? " · " + qsTr("第二次 %1").arg(secondLabel) : "")
                            // Base target, then the supersample shrink (RTX Video SR), or the double SR
                            // reduction and second target while double SR is on (any algorithm), then the
                            // tier and the second algorithm. Each kept value is hidden while its mode is off.
                            readonly property bool rtxVideoSr: oviora.srAlgorithmOf(oviora.videoSrQuality).startsWith("rtx")
                            readonly property bool supersampleShown: !oviora.dualSrEnabled && oviora.srOutput > 0 && rtxVideoSr
                            readonly property bool dualShown: oviora.dualSrEnabled && oviora.srPrescale > 0
                            // "高码率 超高" for an RTX family, the algorithm name for DLSS SR / FSR.
                            readonly property string secondLabel: oviora.srQualityLabel(oviora.dualSrQuality, true)
                                || oviora.srBackendChoices?.find(o => o.id === oviora.srAlgorithmOf(oviora.dualSrQuality))?.label || ""
                            on: oviora.srEnabled
                            open: true
                            onToggled: on => oviora.srEnabled = on

                            VRow {
                                label: qsTr("超分算法")
                                VSelect {
                                    objectName: "list-sr-backend"
                                    implicitWidth: 210 // the RTX Video SR family names need more than the default 150
                                    options: oviora.srBackendChoices || []
                                    value: options.find(o => o.id === oviora.srAlgorithmOf(oviora.videoSrQuality))?.label || ""
                                    // Switching between RTX families keeps the tier.
                                    onPicked: id => oviora.videoSrQuality = oviora.srQualityFor(id, oviora.srTierOf(oviora.videoSrQuality))
                                }
                            }
                            VRow {
                                label: qsTr("DLSS 模型")
                                hint: qsTr("K 保留原有模型；M、L 为 DLSS 4.5。L 针对较低输入分辨率，实际画质与耗时取决于片源。")
                                // Also the model of a DLSS SR second pass under double SR.
                                visible: oviora.videoSrQuality === 0 || (oviora.dualSrEnabled && oviora.dualSrQuality === 0)
                                enabled: Support.available(oviora, "sr0")
                                VSelect {
                                    objectName: "list-dlss-sr-model"
                                    options: oviora.dlssSrModelChoices || []
                                    value: options.find(o => o.id === String(oviora.dlssSrModel))?.label || ""
                                    onPicked: id => oviora.dlssSrModel = Number(id)
                                }
                            }
                            SrStageEditor { Layout.fillWidth: true; namePrefix: "list" }


                        }

                        // RTX Video HDR: SDR -> HDR before frame generation. Runs
                        // only on an HDR display path; the status says why not.
                        VAccordion {
                            id: videoHdrCard
                            objectName: "list-video-hdr"
                            Layout.fillWidth: true
                            glyph: "sun"
                            hue: "#E58BD9"
                            title: "RTX Video HDR"
                            switchAvailable: Support.available(oviora, "video-hdr")
                            summary: !switchAvailable ? Support.reason(oviora, "video-hdr")
                                     : oviora.videoHdr && oviora.displayHdr === 0 ? qsTr("Windows HDR kapalı: çalışmıyor")
                                     : oviora.videoHdr ? (oviora.videoHdrStatus.length > 0 ? oviora.videoHdrStatus : qsTr("已开启")) : qsTr("已关闭")
                            on: oviora.videoHdr
                            onToggled: on => oviora.videoHdr = on
                            // RTX Video HDR is an SDR -> HDR stage: an HDR source
                            // keeps the native HDR path (and the HDR brightness
                            // management), so there is no "convert HDR sources" switch.
                            Repeater {
                                model: [{key:"contrast",label:qsTr("对比度"),from:0,to:200,def:125},
                                        {key:"saturation",label:qsTr("饱和度"),from:0,to:200,def:75},
                                        {key:"middleGray",label:qsTr("中灰"),from:10,to:100,def:44},
                                        {key:"peakNits",label:qsTr("峰值亮度 (nit)"),from:400,to:2000,def:1000}]
                                delegate: VRow {
                                    required property var modelData
                                    enabled: Support.available(oviora, "video-hdr")
                                    opacity: enabled ? 1 : 0.4
                                    label: modelData.label
                                    // modelData.scale turns an integer parameter into a
                                    // readable number (exposure is 1/100 EV); the unit
                                    // lives in the label, so every value stays short
                                    // enough for the rows to line up.
                                    value: modelData.scale
                                        ? (Number(oviora.videoHdrParams[modelData.key] ?? modelData.def) / modelData.scale).toFixed(2)
                                        : String(oviora.videoHdrParams[modelData.key] ?? "—")
                                    VSlider {
                                        objectName: "list-hdr-" + modelData.key
                                        implicitWidth: 120
                                        valueFromModel: true
                                        resettable: true; defaultValue: modelData.def
                                        from: modelData.from; to: modelData.to
                                        value: oviora.videoHdrParams[modelData.key] ?? modelData.def
                                        onMoved: value => oviora.setVideoHdrParameter(modelData.key, Math.round(value))
                                    }
                                }
                            }
                        }

                        // RTX Video HDR turns SDR into HDR, which only an HDR desktop shows: with Windows
                        // HDR off (display-query hdr=off) the stage is never built, so say what to do.
                        Rectangle {
                            objectName: "list-video-hdr-needs-hdr"
                            Layout.fillWidth: true
                            visible: oviora.videoHdr && oviora.displayHdr === 0
                            implicitHeight: hdrOffText.implicitHeight + 20
                            radius: 10
                            color: Theme.warnGlow
                            border.width: 1
                            border.color: Qt.rgba(1, 0.722, 0.420, 0.45)
                            Text {
                                textFormat: Text.PlainText
                                id: hdrOffText
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                text: qsTr("Windows HDR kapalı; bu özellik yalnızca HDR açıkken çalışır. Windows Ayarları > Ekran > HDR kullan")
                                wrapMode: Text.WordWrap
                                color: Theme.warn
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fsSmall
                            }
                        }

                        // .stage: one NR card, its layers inside (pages-pro.js nrStage):
                        // "N 层依次处理 · 每层参数独立", a bar per layer lit when it
                        // runs, the add button at the bottom of the stage.
                        VAccordion {
                            id: nrStage
                            objectName: "list-nr-stage"
                            Layout.fillWidth: true
                            glyph: "wand"
                            hue: Theme.accent
                            title: qsTr("NR 画面增强")
                            // Oviora: NR needs the DLSS NR add-on (Ayarlar > Görüntü > Ek bileşenler)
                            summary: !Support.available(oviora, "nr") && !(platform.components.dlssnr || {}).installed
                                ? qsTr("İndirilmedi · Ayarlar > Görüntü > Ek bileşenler'den indir")
                                : oviora.nrLayers.length > 0 ? oviora.nrLayers.length + qsTr(" 层依次处理 · 每层参数独立") : qsTr("未添加 NR 层")
                            // Master switch (field request 2026-10-02): every layer off
                            // at once; on again brings back the layers that were on.
                            enabledSwitch: oviora.nrLayers.length > 0
                            switchAvailable: Support.available(oviora, "nr")
                            switchObjectName: "list-nr-master"
                            on: oviora.nrAnyEnabled
                            bypassed: oviora.nrLayers.length > 0 && !oviora.nrAnyEnabled
                            onToggled: on => oviora.setAllNrEnabled(on)
                            open: true
                            headerActions: [
                                VTag { text: qsTr("实验"); kind: "exp" },
                                Row {
                                    objectName: "list-nr-count"
                                    spacing: 3
                                    Repeater {
                                        model: oviora.nrLayers
                                        delegate: Rectangle {
                                            required property var modelData
                                            width: 14; height: 4; radius: 3
                                            color: modelData.enabled ? Theme.accent : Qt.rgba(1, 1, 1, 0.18)
                                            Behavior on color { ColorAnimation { duration: Theme.d(200) } }
                                        }
                                    }
                                }
                            ]
                            Repeater {
                                // Numeric count preserves delegates during slider edits.
                                model: oviora.nrLayers.length
                                delegate: NrLayerCard {
                                    id: layerCard
                                    required property int index
                                    Layout.topMargin: 4
                                    layerData: oviora.nrLayers[index]
                                    layerNumber: index + 1
                                    layerCount: oviora.nrLayers.length
                                    nrFirst: oviora.lowLatency
                                    onEdited: (nodeIndex,key,amount) => oviora.setNrLayerParameter(nodeIndex,key,amount)
                                    onEnabledEdited: (nodeIndex,enabled) => oviora.setEffectEnabled(nodeIndex,enabled)
                                    onDuplicateRequested: nodeIndex => { root.markPop(layerNumber + 1); root.selectedEffect = oviora.duplicateNrLayer(nodeIndex) }
                                    onRemoveRequested: nodeIndex => layerCard.bye(() => oviora.removeEffect(nodeIndex))
                                    onOrderEdited: enabled => oviora.lowLatency = enabled
                                }
                            }

                            // .addlayer: dashed, inside the stage; list mode adds and
                            // toggles layers, never reorders them.
                            AddLayerButton {
                                objectName: "list-nr-add"
                                full: oviora.nrLayers.length >= 4
                                label: qsTr("+  添加 NR 层")
                                note: full ? qsTr("（最多 4 层）") : qsTr("（加在 NR ") + oviora.nrLayers.length + qsTr(" 之后 · 最多 4 层）")
                                onAdd: { root.markPop(oviora.nrLayers.length + 1); root.selectedEffect = oviora.addEffect("nr") }
                            }

                            // Global NR options sit inside the NR stage, beside its layers
                            // (field request 2026-11-07): each runs once over the whole stack.
                            // Stage-5 output stabiliser. Global, not per-layer: it
                            // runs once after the whole NR stack. Default off, and
                            // off means the pass is never even dispatched.
                            VAccordion {
                                objectName: "list-output-stabiliser"
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                title: qsTr("输出稳定器 · 抗闪烁")
                                glyph: "shield"; hue: Theme.accent
                                summary: oviora.nrHoldStrength > 0
                                         ? qsTr("已开启 · 强度 ") + Math.round(oviora.nrHoldStrength * 100) + "%"
                                         : qsTr("默认关闭 · 只稳住没变化的画面，动的地方不拖影")
                                on: oviora.nrHoldStrength > 0
                                onToggled: on => oviora.nrHoldStrength = on ? 0.8 : 0
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: qsTr("比较源画面与上一帧：没有明显变化的像素沿用上一帧输出，明显变化的位置直接用新结果。") +
                                          qsTr("不重投影、不用光流，所以运动区域不会拖影。放在最后一层 NR 之后、超分与补帧之前。")
                                    wrapMode: Text.WordWrap
                                    color: Theme.t3
                                    font.family: Theme.fontUi; font.pixelSize: 11
                                }
                                VRow {
                                    label: qsTr("稳定强度")
                                    hint: qsTr("越高越稳，过高会显得发闷")
                                    visible: oviora.nrHoldStrength > 0
                                    value: Math.round(oviora.nrHoldStrength * 100) + "%"
                                    VSlider {
                                        objectName: "list-hold-strength"
                                        implicitWidth: 110
                                        from: 0.1; to: 1.0; inputScale: 100
                                        value: oviora.nrHoldStrength
                                        onMoved: value => oviora.nrHoldStrength = value
                                    }
                                }
                                VRow {
                                    label: qsTr("变化容差")
                                    hint: qsTr("越小越敏感，越大稳得越多")
                                    visible: oviora.nrHoldStrength > 0
                                    value: Number(oviora.nrHoldTolerance).toFixed(3)
                                    VSlider {
                                        objectName: "list-hold-tolerance"
                                        implicitWidth: 110
                                        inputDecimals: 3
                                        from: 0.005; to: 0.10
                                        value: oviora.nrHoldTolerance
                                        onMoved: value => oviora.nrHoldTolerance = value
                                    }
                                }
                            }

                            // NR inference reuse (src/pipeline/NrReuse.h). Global like the
                            // stabiliser and realtime preview only; default off, and off
                            // builds no reuse pass at all.
                            VAccordion {
                                objectName: "list-nr-reuse"
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                title: qsTr("NR 自动推理复用")
                                glyph: "zap"; hue: Theme.accent
                                summary: oviora.nrReuseMode === 0
                                         ? qsTr("默认关闭 · 跟不上时让部分帧沿用上一帧的 NR 结果")
                                         : oviora.nrReuseStatus.length > 0 ? oviora.nrReuseStatus
                                         : oviora.nrReuseMode === 1 ? qsTr("已开启 · 自动")
                                         : qsTr("已开启 · 每 ") + oviora.nrReuseMode + qsTr(" 帧复用 1 帧")
                                on: oviora.nrReuseMode !== 0
                                onToggled: on => oviora.nrReuseMode = on ? 1 : 0
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: qsTr("只在实时预览生效：被复用的帧不跑 NR 推理，改用上一帧的 NR 变化按光流对齐后叠加，下一帧一定重新推理；对不齐的地方显示原画面。") +
                                          qsTr("导出、暂停单帧和图片照常逐帧完整推理。目前支持英伟达 NR、SDR 和列表模式。")
                                    wrapMode: Text.WordWrap
                                    color: Theme.t3
                                    font.family: Theme.fontUi; font.pixelSize: 11
                                }
                                VRow {
                                    label: qsTr("复用方式")
                                    hint: qsTr("自动：只在跟不上时复用，最多每 2 帧复用 1 帧；手动：固定比例")
                                    visible: oviora.nrReuseMode !== 0
                                    VSelect {
                                        objectName: "list-nr-reuse-mode"
                                        implicitWidth: 210
                                        options: [
                                            { id: "1", label: qsTr("自动") },
                                            { id: "2", label: qsTr("手动 1/2 · 每 2 帧复用 1 帧") },
                                            { id: "3", label: qsTr("手动 1/3 · 每 3 帧复用 1 帧") },
                                            { id: "4", label: qsTr("手动 1/4 · 每 4 帧复用 1 帧") }
                                        ]
                                        value: (options.find(o => o.id === String(oviora.nrReuseMode)) || options[0]).label
                                        onPicked: id => oviora.nrReuseMode = Number(id)
                                    }
                                }
                            }

                            VAccordion {
                                id: protectionCard
                                objectName: "list-global-protection"
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                title: qsTr("NR 全局保护区域")
                                glyph: "shield"; hue: Theme.accent
                                summary: qsTr("区域内排除所有 NR 层") + (oviora.shortcuts.maskToggle ? " · " + oviora.shortcuts.maskToggle : "")
                                on: oviora.protectionEnabled
                                onToggled: on=>oviora.protectionEnabled=on
                                MaskEditor { namePrefix: "list-protection" }
                            }
                        }

                        VAccordion {
                            id: exclusionCard
                            objectName: "list-effect-exclusion"
                            Layout.fillWidth: true
                            title: qsTr("全效果剔除")
                            glyph: "shield"; hue: Theme.accent
                            summary: qsTr("区域内恢复原始画面与帧率") + (oviora.shortcuts.maskToggle ? " · " + oviora.shortcuts.maskToggle : "")
                            on: oviora.exclusionState.enabled===true
                            onToggled: on=>oviora.setMaskEnabled(true,on)
                            MaskEditor { allEffects: true; namePrefix: "list-exclusion" }
                        }

                        // A refused edit explains itself here rather than in a modal.
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            visible: !oviora.chainValid
                            text: oviora.chainError
                            color: Theme.err
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsSmall
                            wrapMode: Text.WordWrap
                        }
                            }
                        }
                    }

                    // --- 补帧 -------------------------------------------------
                    // pages-pro.js fg tab: one 补帧 card with its switch ("始终在所有
                    // 画质效果之后"), the optical-flow and presentation groups nested
                    // as sub-panels. Backend choices share the node editor's
                    // bridge contract; FSR 4 requests never silently become 3.1.
                    Loader {
                        id: fgLoader
                        Layout.fillWidth: true
                        visible: root.tab === "fg"
                        active: false
                        Component.onCompleted: if (root.tab === "fg") active = true
                        sourceComponent: Component {
                            ColumnLayout {
                                id: fgCol
                                spacing: 6
                                objectName: "list-fg-group"

                        VAccordion {
                            id: fgCard
                            objectName: "list-fg-card"
                            Layout.fillWidth: true
                            glyph: "layers"
                            hue: "#3DDC84"
                            title: qsTr("补帧")
                            summary: qsTr("始终在所有画质效果之后") + (oviora.fgEnabled
                                     ? " · " + oviora.fgMultiplier + "X " + (oviora.fgBackendChoices.find(o => o.id === oviora.fgBackendName) || {}).label
                                     : qsTr(" · 已关闭"))
                            switchObjectName: "list-fg-enabled"
                            switchAvailable: Support.available(oviora, "frame-generation")
                            on: oviora.fgEnabled
                            onToggled: on => oviora.fgEnabled = on
                            open: true

                            VRow {
                                label: qsTr("补帧方式")
                                hint: oviora.fgProviderText
                                VSelect {
                                    id: fgBackendSelect
                                    objectName: "list-fg-backend"
                                    // Oviora: only methods this package and this PC can run (VFG stays: Ek bileşenler)
                                    options: oviora.fgBackendChoices.filter(o => !o.disabled || o.id === "vfg")
                                    // The label of the chosen backend, not its id.
                                    value: (options.find(o => o.id === oviora.fgBackendName) || options[0]).label
                                    onPicked: id => oviora.fgBackendName = id
                                }
                            }
                            VRow {
                                label: qsTr("补帧运动来源")
                                visible: oviora.fgBackendName !== "vfg"
                                hint: qsTr("XeSS 默认零运动；FSR 仍有自身的光流计算")
                                VSeg { objectName: "list-fg-motion"; options: [{id:"0",label:qsTr("零运动")},{id:"1",label:qsTr("光流")}]; current: String(oviora.fgMotionSource); onPicked: id => oviora.fgMotionSource = Number(id) }
                            }
                            VRow {
                                label: qsTr("倍率")
                                // Live output hint: a suggestion, the multiplier stays as chosen.
                                hint: oviora.fgDisplayHint.length > 0 ? oviora.fgDisplayHint : oviora.fgMaxMultiplier + qsTr("X 为上限")
                                hintColor: oviora.fgDisplayHint.length > 0 ? Theme.warn : Theme.t3
                                VSeg {
                                    visible: oviora.fgBackendName !== "vfg"
                                    options: oviora.fgMultiplierChoices
                                    current: String(oviora.fgMultiplier)
                                    onPicked: id => oviora.fgMultiplier = parseInt(id)
                                }
                                VSelect {
                                    objectName: "list-vfg-multiplier"
                                    visible: oviora.fgBackendName === "vfg"
                                    options: oviora.fgMultiplierChoices
                                    value: String(oviora.fgMultiplier) + "X"
                                    onPicked: id => oviora.fgMultiplier = Number(id)
                                }
                            }
                            // Oviora: the game's frame rate (30 / 60), right under the multiplier
                            DedupModeRow { backend: oviora; namePrefix: "list" }
                            VRow {
                                label: qsTr("DLSS 性能模式")
                                visible: oviora.fgBackendName === "dlss"
                                enabled: oviora.dlssgLiteAvailable
                                opacity: enabled ? 1 : 0.5
                                hint: oviora.dlssgLiteHint
                                VSwitch {
                                    objectName: "list-dlssg-lite"
                                    checked: oviora.dlssgLite
                                    onToggled: checked => oviora.dlssgLite = checked
                                }
                            }
                            VRow {
                                label: qsTr("VFG 质量")
                                visible: oviora.fgBackendName === "vfg"
                                hint: qsTr("高质量更耗时；混合档：每对原帧的中间帧用前一档，其余用后一档")
                                VSelect {
                                    objectName: "list-vfg-quality"
                                    options: [{id:"0",label:qsTr("低")},{id:"1",label:qsTr("中")},{id:"2",label:qsTr("高")},{id:"3",label:qsTr("高+中")},{id:"4",label:qsTr("高+低")},{id:"5",label:qsTr("中+低")}]
                                    value: options[oviora.vfgQuality].label
                                    onPicked: id => oviora.vfgQuality = Number(id)
                                }
                            }
                            VRow {
                                label: qsTr("VFG 自动调控")
                                visible: oviora.fgBackendName === "vfg"
                                enabled: oviora.vfgQuality !== 0 && oviora.fgMultiplier === 2
                                opacity: enabled ? 1 : 0.5
                                hint: qsTr("实时采集 2X 时，当前档来不及赶上屏幕刷新，这一对帧就改用下一档，不丢帧也不加延迟。多占约 2 GB 显存，显存不够时不生效")
                                VSwitch {
                                    objectName: "list-vfg-auto-regulate"
                                    checked: oviora.vfgAutoRegulate
                                    onToggled: checked => oviora.vfgAutoRegulate = checked
                                }
                            }
                            VRow {
                                label: qsTr("严格补帧节奏")
                                hint: qsTr("帧同步 · 默认关闭")
                                VSwitch {
                                    checked: oviora.fgStrict
                                    onToggled: checked => oviora.fgStrict = checked
                                }
                            }
                            VSubGroup {
                                objectName: "list-fg-flow-group"
                                Layout.fillWidth: true
                                Layout.topMargin: 6
                                label: qsTr("光流 · 运动估算")
                                count: oviora.opticalFlowChoice === 1 ? 5 : 4
                                expanded: true
                                VRow {
                                    label: qsTr("光流算法")
                                    hint: qsTr("补帧与 NR 共用 · 输入后计算一次")
                                    VSelect {
                                        objectName: "list-flow-choice"
                                        options: [Support.option(oviora,"0","NVIDIA NVOF","flow0"),Support.option(oviora,"1","AMD FidelityFX","flow1")]
                                        value: options[oviora.opticalFlowChoice]?.label ?? (oviora.opticalFlowChoice === 2 ? qsTr("GPU DIS 已移除，请重选") : qsTr("未知配置"))
                                        onPicked: id => oviora.setOpticalFlowChoice(Number(id))
                                    }
                                }
                                VRow {
                                    label: qsTr("边缘神盾")
                                    hint: qsTr("DLSS、FSR、ANR 补帧，及运动来源选“光流”的 XeSS 生效：修人物边缘果冻，背景不再被人物拖着晃")
                                    VSwitch {
                                        objectName: "list-edge-aegis"
                                        checked: oviora.edgeAegis
                                        onToggled: checked => oviora.edgeAegis = checked
                                    }
                                }
                                VRow {
                                    label: qsTr("AMD 性能档")
                                    hint: qsTr("光流宽高减半，更省但更糙")
                                    visible: oviora.opticalFlowChoice === 1
                                    VSwitch {
                                        objectName: "list-amd-half"
                                        checked: oviora.amdFlowHalf
                                        onToggled: checked => oviora.amdFlowHalf = checked
                                    }
                                }
                                VRow {
                                    label: qsTr("运动估算质量")
                                    VSeg {
                                        objectName: "list-flow-quality"
                                        options: [{ id: "0", label: qsTr("性能") }, { id: "1", label: qsTr("平衡") }, { id: "2", label: qsTr("质量") }]
                                        current: String(oviora.flowQuality)
                                        onPicked: id => oviora.flowQuality = Number(id)
                                    }
                                }
                            }
                            NvidiaLowLatencyEditor { namePrefix: "list" }
                            VRow {
                                visible: !oviora.nvidiaLowLatencyAvailable
                                label: qsTr("低延迟队列")
                                hint: qsTr("减少处理排队")
                                VSwitch {
                                    objectName: "list-low-queue"
                                    checked: oviora.fgLowQueue
                                    onToggled: checked => oviora.fgLowQueue = checked
                                }
                            }
                        }
                            }
                        }
                    }

                    // --- 色彩 -------------------------------------------------
                    // The same real-instance editor is used by the node inspector.
                    Loader {
                        id: colourLoader
                        Layout.fillWidth: true
                        visible: root.tab === "color"
                        active: false
                        Component.onCompleted: if (root.tab === "color") active = true
                        sourceComponent: Component {
                            ColourPanel {
                                id: colourPanel
                                api: oviora
                            }
                        }
                    }

                    // --- 声音 -------------------------------------------------
                    Loader {
                        id: audioLoader
                        Layout.fillWidth: true
                        visible: root.tab === "audio"
                        active: false
                        Component.onCompleted: if (root.tab === "audio") active = true
                        sourceComponent: Component {
                            ProAudioPanel {
                                id: audioCol
                            }
                        }
                    }

                    // --- 显示 -------------------------------------------------
                    Loader {
                        id: displayLoader
                        Layout.fillWidth: true
                        visible: root.tab === "display"
                        active: false
                        Component.onCompleted: if (root.tab === "display") active = true
                        sourceComponent: Component {
                            ProDisplayPanel {
                                id: displayCol
                            }
                        }
                    }

                    Item { Layout.preferredHeight: 12 }
                }
            }

            // .insp-foot
            RowLayout {
                objectName: "pro-inspector-footer"
                Layout.fillWidth: true
                Layout.margins: 10
                spacing: 8
                VDot { Layout.alignment: Qt.AlignTop; Layout.topMargin: 5; off: !oviora.running; err: oviora.failed }
                Text {
                    textFormat: Text.PlainText
                    objectName: "pro-inspector-status"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    // Without a stream the engine's idle line means nothing here.
                    text: oviora.hasSource ? oviora.statusText : ""
                    color: Theme.t2
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall
                    wrapMode: Text.Wrap
                }
                VButton {
                    objectName: "pro-reset-current-page"
                    Layout.alignment: Qt.AlignTop
                    ghost: true
                    text: qsTr("重置本页")
                    onClicked: oviora.resetCurrentPage()
                }
            }
        }
    }

    // pages-pro.js layer "更多": copy as a new layer, back to defaults, remove.
    VMenu {
        id: nrLayerMenu
        objectName: "nr-layer-menu"
        property Item layer: null
        title: layer ? qsTr("NR 层 ") + layer.layerNumber : ""
        items: layer ? [
            { label: qsTr("复制为新层"), icon: "dup", act: "dup", disabled: oviora.nrLayers.length >= 4 },
            { label: qsTr("恢复默认"), icon: "reset", act: "reset" },
            { label: qsTr("删除这一层"), icon: "trash", act: "del", disabled: oviora.nrLayers.length <= 1 }
        ] : []
        onPicked: (i, o) => {
            const card = layer
            if (!card) return
            const nodeIndex = card.layerData.index
            if (o.act === "dup") { root.markPop(card.layerNumber + 1); root.selectedEffect = oviora.duplicateNrLayer(nodeIndex) }
            else if (o.act === "reset") oviora.resetNrLayer(nodeIndex)
            else if (o.act === "del") card.bye(() => oviora.removeEffect(nodeIndex))
        }
    }

    VRise { target: insp; d: 2 }
}
