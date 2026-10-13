// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 1240; height: 820; visible: true
    title: (documentController.hasDocument ? documentController.title + " — " : "") + "yolopdf"
    readonly property var doc: documentController.document
    readonly property var pages: doc.pages || []
    readonly property bool mobile: width < 760
    property string tool: "Select"
    property var selectedPages: []
    property var selectedObject: null
    property string selectionKind: ""
    property real zoom: 1
    property string zoomMode: "Fit width"
    property bool overviewVisible: !mobile
    property bool darkTheme: false
    property bool snapping: true
    property var searchIndex: -1
    property var sourceToOpen: ""
    property string tabToClose: ""
    property bool closeAfterSave: false
    property bool allowQuit: false
    property string imagePurpose: "insert"
    property string fontPurpose: "add"
    property url imageArtwork: ""
    property url stampArtwork: ""
    property url textFont: ""
    property real stampWidth: 40
    property real stampHeight: 20
    property real stampRotation: 0
    property real stampOpacity: 1
    property int stampFrame: 0
    property point stampPointer: Qt.point(0,0)
    property var area: null
    palette.window: darkTheme ? "#19212b" : "#edf0f4"
    palette.base: darkTheme ? "#242e3b" : "#ffffff"
    palette.text: darkTheme ? "#f4f6fa" : "#1c293a"
    palette.windowText: darkTheme ? "#f4f6fa" : "#1c293a"
    palette.button: darkTheme ? "#303e50" : "#f7f9fc"
    palette.buttonText: darkTheme ? "#f4f6fa" : "#1c293a"
    palette.highlight: "#346bdb"
    palette.highlightedText: "#ffffff"
    color: palette.window

    function enabledEdit() { return documentController.hasDocument && !documentController.busy && doc.mayEdit }
    function choosePage(index,modifiers) {
        let list = selectedPages.slice()
        if (modifiers & Qt.ControlModifier) {
            let at=list.indexOf(index); if(at<0)list.push(index);else list.splice(at,1)
        } else if((modifiers & Qt.ShiftModifier) && list.length) {
            let first=list[0];list=[];for(let i=Math.min(first,index);i<=Math.max(first,index);++i)list.push(i)
        } else list=[index]
        selectedPages=list.sort((a,b)=>a-b)
        documentController.selectPage(index)
    }
    function pickObject(kind,item) {selectionKind=kind;selectedObject=item}
    function refreshObject() {
        if(!selectedObject)return
        let list=selectionKind==="Image" ? (doc.images||[]) : selectionKind==="Text" ? (doc.runs||[]) : (doc.annotations||[])
        for(let item of list)if(item.id===selectedObject.id){selectedObject=item;return}
        selectedObject=null
    }
    function moveObject(x,y,w,h) {
        if(!selectedObject)return
        if(selectionKind==="Image") {
            let s=selectedObject, m=s.matrix.slice(),sx=w/s.width,sy=h/s.height
            m[0]*=sx;m[2]*=sx;m[1]*=sy;m[3]*=sy
            m[4]=x+(m[4]-s.x)*sx;m[5]=y+(m[5]-s.y)*sy
            documentController.transformImage(s.id,m)
        } else if(selectionKind==="Annotation") documentController.moveAnnotation(selectedObject.id,x,y,w,h)
    }
    function nudge(dx,dy) { if(selectedObject && selectionKind!=="Text") moveObject(selectedObject.x+dx,selectedObject.y+dy,selectedObject.width,selectedObject.height) }
    function snap(v,axis,page) {
        if(!snapping)return v
        let values=[0,(axis==="x"?page.width:page.height)/2,axis==="x"?page.width:page.height]
        for(let item of (doc.images||[]).concat(doc.annotations||[])) {
            values.push(axis==="x"?item.x:item.y)
            values.push(axis==="x"?item.x+item.width:item.y+item.height)
        }
        let best=v, distance=4
        for(let candidate of values)if(Math.abs(candidate-v)<distance){best=candidate;distance=Math.abs(candidate-v)}
        return best
    }
    function editRun(run,pageItem,scale) {
        pickObject("Text",run)
        if(run.status==="Unsupported")return
        textPopup.run=run; textPopup.fontFile=""; textPopup.face=0; textPopup.sizeChanged=false
        editor.text=run.text; editSize.text=String(run.size);editColor.text="";allowWider.checked=false;wrapText.checked=false;wrapWidthMm.text=(run.width*25.4/72).toFixed(2)
        let p=pageItem.mapToItem(window.contentItem,run.x*scale,run.y*scale)
        textPopup.x=Math.max(4,Math.min(window.width-textPopup.width-4,p.x))
        textPopup.y=Math.max(4,Math.min(window.contentItem.height-textPopup.height-4,p.y))
        textPopup.open();editor.forceActiveFocus();editor.selectAll()
    }
    function findNext() {
        if(!documentController.searchHits.length)return
        searchIndex=(searchIndex+1)%documentController.searchHits.length
        let hit=documentController.searchHits[searchIndex];documentController.selectPage(hit.page)
        pageView.positionViewAtIndex(hit.page,ListView.Center)
    }
    function saveDocument() {let source=doc.source;if(source && String(source)!=="" && !String(source).startsWith("qrc:"))documentController.save();else saveDialog.open()}
    Shortcut {sequences: [StandardKey.Open]; onActivated:openDialog.open()}
    Shortcut {sequences: [StandardKey.Save];enabled:documentController.hasDocument&&!documentController.busy;onActivated:window.saveDocument()}
    Shortcut {sequences: [StandardKey.SaveAs];enabled:documentController.hasDocument;onActivated:saveDialog.open()}
    Shortcut {sequences: [StandardKey.Undo];enabled:!!doc.canUndo&&!documentController.busy&&!textPopup.opened;onActivated:documentController.undo()}
    Shortcut {sequences: [StandardKey.Redo];enabled:!!doc.canRedo&&!documentController.busy&&!textPopup.opened;onActivated:documentController.redo()}
    Shortcut {sequences: [StandardKey.Close];enabled:documentController.hasDocument;onActivated:documentController.closeTab(doc.id)}
    Shortcut {sequences: [StandardKey.Find];onActivated:searchField.forceActiveFocus()}
    Shortcut {sequence:"Escape";onActivated:{textPopup.close();documentController.cancelPreview();tool="Select";selectedObject=null;area=null}}
    onClosing: function(close) {
        if(!allowQuit && documentController.tabs.some(t=>t.dirty)){close.accepted=false;quitDialog.open()}
    }
    Connections {
        target: documentController
        function onChanged() {
            window.refreshObject()
            if(!selectedPages.length && documentController.hasDocument)selectedPages=[doc.page]
        }
        function onPasswordRequired(source) {sourceToOpen=source;passwordField.clear();passwordDialog.open()}
        function onInstalledFontFound(file,face) {textPopup.fontFile=file;textPopup.face=face;previewTimer.restart()}
        function onSignatureRequested() {signatureDialog.open()}
        function onCloseRequested(id) {tabToClose=id;closeDialog.open()}
        function onSaved(ok) {if(ok&&closeAfterSave){closeAfterSave=false;documentController.closeTab(tabToClose)}}
    }
    Connections {
        target:assetLibrary
        function onStampImported(id) {
            let items=assetLibrary.stamps
            for(let item of items)if(item.id===id){stampArtwork=item.file;stampWidth=item.widthMm;stampHeight=item.heightMm;stampFrame=item.gifFrame;tool="Stamp";return}
        }
        function onFontImported(file) {if(fontPurpose==="edit"){textPopup.fontFile=file;previewTimer.restart()}else textFont=file}
    }
    header: ColumnLayout {
        spacing:0
        ToolBar {
            Layout.fillWidth:true
            RowLayout {
                anchors.fill:parent;anchors.margins:4
                ToolButton {text:"Open";Accessible.name:"Open document";Layout.minimumHeight:48;enabled:!documentController.busy;onClicked:openDialog.open()}
                ToolButton {text:"Save";Accessible.name:"Save document";Layout.minimumHeight:48;enabled:documentController.hasDocument&&!documentController.busy;onClicked:window.saveDocument()}
                ToolButton {text:"Save As";Accessible.name:text;Layout.minimumHeight:48;enabled:documentController.hasDocument&&!documentController.busy;visible:!mobile;onClicked:saveDialog.open()}
                ToolButton {text:"Undo";Accessible.name:doc.undoLabel ? "Undo " + doc.undoLabel : text;Layout.minimumHeight:48;enabled:!!doc.canUndo&&!documentController.busy;onClicked:documentController.undo()}
                ToolButton {text:"Redo";Accessible.name:doc.redoLabel ? "Redo " + doc.redoLabel : text;Layout.minimumHeight:48;enabled:!!doc.canRedo&&!documentController.busy;visible:!mobile;onClicked:documentController.redo()}
                Item {Layout.fillWidth:true}
                ToolButton {text:"Pages";Accessible.name:"Toggle page overview";Layout.minimumHeight:48;checkable:true;checked:overviewVisible;onClicked:overviewVisible=checked}
                ToolButton {text:"⋯";Accessible.name:"More actions";Layout.minimumHeight:48;onClicked:moreMenu.open()
                    Menu {id:moreMenu
                        MenuItem {text:"Save As…";onTriggered:saveDialog.open();enabled:documentController.hasDocument}
                        MenuItem {text:"Open sample";onTriggered:documentController.openSample()}
                        MenuItem {text:"Print…";enabled:documentController.hasDocument&&!!doc.mayPrint;onTriggered:printSettings.open()}
                        MenuItem {text:"Flatten stamps";onTriggered:documentController.flattenStamps();enabled:window.enabledEdit()}
                        MenuItem {text:"Dark theme";checkable:true;checked:darkTheme;onTriggered:darkTheme=checked}
                        MenuItem {text:"Close document";enabled:documentController.hasDocument;onTriggered:documentController.closeTab(doc.id)}
                    }
                }
            }
        }
        Flickable {
            Layout.fillWidth:true;Layout.preferredHeight:48;contentWidth:modes.width;clip:true
            Row {id:modes;spacing:4
                Repeater {model:["Select","Edit text","Add text","Image","Stamp","Annotate"]
                    delegate:Button {required property string modelData;text:modelData;height:48;checkable:true;checked:tool===modelData;Accessible.name:modelData+" tool";onClicked:{tool=modelData;if(modelData==="Stamp"&&stampArtwork===""){if(assetLibrary.stamps.length)collectionDialog.open();else stampDialog.open()};if(modelData==="Add text"&&textFont==="")addFontDialog.open()}}
                }
            }
        }
        TabBar {
            Layout.fillWidth:true;visible:documentController.tabs.length>0
            currentIndex:documentController.tabs.findIndex(t=>t.id===doc.id)
            Repeater {model:documentController.tabs
                delegate:TabButton {required property var modelData;text:modelData.title+(modelData.dirty?" *":"");Accessible.name:text;onClicked:{selectedPages=[];selectedObject=null;documentController.activateTab(modelData.id)}}
            }
        }
    }
    ColumnLayout {
        anchors.fill:parent;spacing:0
        Rectangle {
            Layout.fillWidth:true;Layout.preferredHeight:errorMessage.implicitHeight+16
            visible:documentController.error!==""||assetLibrary.error!=="";color:darkTheme?"#662525":"#ffe2df"
            Label {id:errorMessage;anchors.fill:parent;anchors.margins:8;text:documentController.error||assetLibrary.error;color:darkTheme?"#fff0ee":"#852a22";wrapMode:Text.Wrap;Accessible.name:text}
        }
        Label {Layout.fillWidth:true;Layout.margins:8;visible:(doc.warnings||[]).length>0;text:(doc.warnings||[]).join("\n");wrapMode:Text.Wrap;color:darkTheme?"#f9db83":"#704e00"}
        RowLayout {
            Layout.fillWidth:true;Layout.preferredHeight:48;visible:documentController.hasDocument;spacing:6
            TextField {id:searchField;Layout.fillWidth:true;Layout.maximumWidth:300;placeholderText:"Find text";Accessible.name:"Find text";onAccepted:{searchIndex=-1;documentController.search(text)}}
            Button {text:"Find";Accessible.name:text;enabled:!documentController.busy;onClicked:{searchIndex=-1;documentController.search(searchField.text)}}
            Button {text:"Next";Accessible.name:"Next search result";enabled:documentController.searchHits.length>0;onClicked:window.findNext()}
            Label {text:documentController.searchHits.length?documentController.searchHits.length+" hits":"";visible:!mobile}
            Item {Layout.fillWidth:true}
            ComboBox {id:zoomChoice;model:["Fit width","Fit page","Actual size","50%","100%","150%","200%","400%"]
                Accessible.name:"Zoom";Layout.preferredWidth:mobile?110:150
                onActivated:{zoomMode=currentText;if(currentText.endsWith("%"))zoom=parseFloat(currentText)/100;else if(currentText==="Actual size")zoom=Screen.pixelDensity*25.4/72}
            }
        }
        RowLayout {
            Layout.fillWidth:true;Layout.fillHeight:true;spacing:0
            Pane {
                Layout.fillHeight:true;Layout.preferredWidth:mobile?120:164;visible:overviewVisible&&documentController.hasDocument;padding:4
                ColumnLayout {
                    anchors.fill:parent;spacing:4
                    RowLayout {Layout.fillWidth:true
                        ToolButton {text:"+";Accessible.name:"Insert pages";enabled:window.enabledEdit();onClicked:insertDialog.open()}
                        ToolButton {text:"↻";Accessible.name:"Rotate selected pages";enabled:window.enabledEdit();onClicked:documentController.rotatePages(selectedPages,90)}
                        ToolButton {text:"⋯";Accessible.name:"Page actions";onClicked:pageMenu.open()
                            Menu {id:pageMenu
                                MenuItem {text:"Insert pages…";enabled:window.enabledEdit();onTriggered:insertDialog.open()}
                                MenuItem {text:"Blank page…";enabled:window.enabledEdit();onTriggered:blankDialog.open()}
                                MenuItem {text:"Duplicate";enabled:window.enabledEdit();onTriggered:documentController.duplicatePages(selectedPages)}
                                MenuItem {text:"Delete";enabled:window.enabledEdit()&&selectedPages.length<pages.length;onTriggered:documentController.deletePages(selectedPages)}
                                MenuItem {text:"Extract…";enabled:selectedPages.length>0;onTriggered:extractDialog.open()}
                                MenuItem {text:"Move/copy to tab…";enabled:documentController.tabs.length>1;onTriggered:transferDialog.open()}
                            }
                        }
                    }
                    ListView {
                        id:thumbnailView;Layout.fillWidth:true;Layout.fillHeight:true;model:pages;spacing:8;clip:true;cacheBuffer:120
                        delegate:Item {
                            id:thumbnail;required property var modelData;required property int index
                            width:thumbnailView.width;height:136
                            Rectangle {anchors.fill:parent;radius:6;color:selectedPages.indexOf(index)>=0?window.palette.highlight:window.palette.base}
                            Image {anchors.top:parent.top;anchors.horizontalCenter:parent.horizontalCenter;anchors.topMargin:6;width:parent.width-16;height:108;asynchronous:true;fillMode:Image.PreserveAspectFit
                                source:{let epoch=documentController.renderEpoch;return documentController.pageSource(thumbnail.index,24)}
                                Accessible.name:"Page "+(index+1)+" thumbnail"}
                            Label {anchors.bottom:parent.bottom;anchors.horizontalCenter:parent.horizontalCenter;text:index+1;color:selectedPages.indexOf(index)>=0?"white":window.palette.text}
                            Rectangle {
                                id:dragProxy;x:0;y:0;width:thumbnail.width;height:thumbnail.height;visible:thumbMouse.drag.active;opacity:.5;color:window.palette.highlight
                                Drag.active:thumbMouse.drag.active;Drag.source:thumbnail;Drag.hotSpot.x:width/2;Drag.hotSpot.y:height/2
                                Drag.mimeData:({"application/x-yolopdf-pages":JSON.stringify(selectedPages)})
                            }
                            MouseArea {id:thumbMouse;anchors.fill:parent;drag.target:dragProxy;onPressed:function(mouse){if(selectedPages.indexOf(index)<0 || selectedPages.length<=1 || (mouse.modifiers&(Qt.ControlModifier|Qt.ShiftModifier)))window.choosePage(index,mouse.modifiers)}
                                onClicked:function(mouse){pageView.positionViewAtIndex(index,ListView.Beginning)}
                                onPressAndHold:window.choosePage(index,Qt.ControlModifier)
                                onReleased:{dragProxy.Drag.drop();dragProxy.x=0;dragProxy.y=0}
                            }
                        }
                        ScrollBar.vertical:ScrollBar {}
                    }
                }
                DropArea {
                    id:overviewDrop;anchors.fill:parent;property int at:pages.length;property string files:""
                    onPositionChanged:function(drag){let i=thumbnailView.indexAt(drag.x,drag.y-thumbnailView.y+thumbnailView.contentY);at=i<0?pages.length:i;files=drag.hasUrls?drag.urls.map(u=>String(u).split('/').pop()).join("\n"):""}
                    onDropped:function(drop){if(drop.hasUrls)documentController.dropFiles(drop.urls,true,at);else if(drop.formats.indexOf("application/x-yolopdf-pages")>=0){try{documentController.movePages(JSON.parse(drop.getDataAsString("application/x-yolopdf-pages")),at)}catch(e){}}drop.acceptProposedAction()}
                    Rectangle {anchors.fill:parent;visible:overviewDrop.containsDrag;color:"#b3346bdb";border.color:window.palette.highlight;border.width:2
                        Label {anchors.centerIn:parent;width:parent.width-12;text:overviewDrop.files?"Insert pages here\n"+overviewDrop.files:"Reorder pages here";wrapMode:Text.Wrap;color:"white";horizontalAlignment:Text.AlignHCenter}
                    }
                }
            }
            Item {
                Layout.fillWidth:true;Layout.fillHeight:true
                DropArea {id:mainDrop;anchors.fill:parent;onDropped:function(drop){if(drop.hasUrls){documentController.dropFiles(drop.urls,false);drop.acceptProposedAction()}}
                    Rectangle {anchors.fill:parent;visible:mainDrop.containsDrag;color:"#b3346bdb";z:20;Label {anchors.centerIn:parent;text:"Open as document";color:"white";font.pixelSize:22}}
                }
                ListView {
                    id:pageView;anchors.fill:parent;model:pages;spacing:20;clip:true;cacheBuffer:600;boundsBehavior:Flickable.StopAtBounds
                    delegate:Item {
                        id:pageDelegate;required property var modelData;required property int index
                        readonly property real scale:zoomMode==="Fit width"?Math.max(.1,(pageView.width-32)/modelData.width):zoomMode==="Fit page"?Math.max(.1,Math.min((pageView.width-32)/modelData.width,(pageView.height-28)/modelData.height)):zoom
                        width:pageView.width;height:modelData.height*scale+32
                        Rectangle {
                            id:paper;width:modelData.width*pageDelegate.scale;height:modelData.height*pageDelegate.scale;anchors.horizontalCenter:parent.horizontalCenter;y:16;color:"white"
                            Image {objectName:pageDelegate.index===0?"pageImage":"pageImageOther";anchors.fill:parent;asynchronous:true;cache:false
                                source:{let epoch=documentController.renderEpoch;return documentController.pageSource(pageDelegate.index,96)}
                                Accessible.name:"PDF page "+(pageDelegate.index+1)
                            }
                            Repeater {
                                model: {
                                    let dpi=Math.min(600,Math.max(24,Math.ceil(72*pageDelegate.scale*Screen.devicePixelRatio)))
                                    let edge=256*72/dpi, logical=edge*pageDelegate.scale
                                    let top=Math.max(0,pageView.contentY-pageDelegate.y-16)
                                    let bottom=Math.min(paper.height,pageView.contentY+pageView.height-pageDelegate.y-16)
                                    let result=[]
                                    for(let row=Math.floor(top/logical);row<Math.ceil(bottom/logical);++row)
                                        for(let column=0;column<Math.ceil(modelData.width/edge);++column)
                                            result.push({x:column*edge,y:row*edge,width:Math.min(edge,modelData.width-column*edge),height:Math.min(edge,modelData.height-row*edge),dpi:dpi})
                                    return result
                                }
                                delegate:Image {
                                    required property var modelData
                                    objectName:"pageTile"
                                    x:modelData.x*pageDelegate.scale;y:modelData.y*pageDelegate.scale
                                    width:modelData.width*pageDelegate.scale;height:modelData.height*pageDelegate.scale
                                    visible:!(textPopup.opened&&pageDelegate.index===doc.page)
                                    asynchronous:true;cache:false
                                    source:{let epoch=documentController.renderEpoch;return documentController.tileSource(pageDelegate.index,modelData.dpi,modelData.x,modelData.y,modelData.width,modelData.height)}
                                }
                            }
                            Repeater {model:documentController.searchHits.filter(h=>h.page===pageDelegate.index)
                                delegate:Rectangle {required property var modelData;x:modelData.x*pageDelegate.scale;y:modelData.y*pageDelegate.scale;width:modelData.width*pageDelegate.scale;height:modelData.height*pageDelegate.scale;color:"#55ffcf22";border.color:"#b48600"}
                            }
                            Repeater {model:pageDelegate.index===doc.page && (tool==="Edit text"||(tool==="Annotate"&&annotationChoice.currentText==="Highlight"))?(doc.runs||[]):[]
                                delegate:Rectangle {required property var modelData;x:modelData.x*pageDelegate.scale;y:modelData.y*pageDelegate.scale;width:Math.max(6,modelData.width*pageDelegate.scale);height:Math.max(6,modelData.height*pageDelegate.scale)
                                    color:textMouse.containsMouse?(modelData.status==="Unsupported"?"#66dc5555":modelData.status==="Needs font"?"#66e5a531":"#55346bdb"):"transparent"
                                    border.color:textMouse.containsMouse?window.palette.highlight:"transparent";z:3
                                    MouseArea {id:textMouse;anchors.fill:parent;hoverEnabled:true;onClicked:{if(tool==="Annotate")documentController.annotate("Highlight",modelData.x,modelData.y,modelData.width,modelData.height);else window.editRun(modelData,paper,pageDelegate.scale)}}
                                    ToolTip.visible:textMouse.containsMouse;ToolTip.text:modelData.status+(modelData.reason?": "+modelData.reason:" · "+modelData.font)
                                }
                            }
                            Rectangle {visible:selectedObject!==null&&pageDelegate.index===doc.page;x:(selectedObject?selectedObject.x:0)*pageDelegate.scale;y:(selectedObject?selectedObject.y:0)*pageDelegate.scale;width:(selectedObject?selectedObject.width:0)*pageDelegate.scale;height:(selectedObject?selectedObject.height:0)*pageDelegate.scale;color:"transparent";border.color:"#346bdb";border.width:2;z:4}
                            Rectangle {visible:area!==null&&pageDelegate.index===doc.page;x:area?area.x*pageDelegate.scale:0;y:area?area.y*pageDelegate.scale:0;width:area?area.width*pageDelegate.scale:0;height:area?area.height*pageDelegate.scale:0;color:"#30346bdb";border.color:"#346bdb";z:4}
                            Rectangle {visible:tool==="Stamp"&&pointer.containsMouse&&stampArtwork!=="";x:stampPointer.x*pageDelegate.scale;y:stampPointer.y*pageDelegate.scale;width:stampWidth*72/25.4*pageDelegate.scale;height:stampHeight*72/25.4*pageDelegate.scale;color:"#55346bdb";border.color:"#346bdb";border.width:2;z:5
                                Image {anchors.fill:parent;source:stampArtwork;opacity:.5;fillMode:Image.PreserveAspectFit}
                            }
                            MouseArea {
                                id:pointer;anchors.fill:parent;hoverEnabled:true;acceptedButtons:Qt.LeftButton;preventStealing:tool!=="Select";z:2
                                property point start;property bool moving:false;property var original:null
                                onPressed:function(mouse){
                                    if(pageDelegate.index!==doc.page){documentController.selectPage(pageDelegate.index);return}
                                    forceActiveFocus();start=Qt.point(mouse.x/pageDelegate.scale,mouse.y/pageDelegate.scale);moving=false
                                    if(tool==="Select" || (tool==="Image"&&imageChoice.currentIndex===0)) {
                                        let chosen=null,kind=""
                                        let images=doc.images||[],annots=doc.annotations||[]
                                        for(let i=annots.length-1;i>=0;--i){let a=annots[i];if(start.x>=a.x&&start.x<=a.x+a.width&&start.y>=a.y&&start.y<=a.y+a.height){chosen=a;kind="Annotation";break}}
                                        if(!chosen)for(let i=images.length-1;i>=0;--i){let a=images[i];if(start.x>=a.x&&start.x<=a.x+a.width&&start.y>=a.y&&start.y<=a.y+a.height){chosen=a;kind="Image";break}}
                                        window.pickObject(kind,chosen);original=chosen;moving=chosen!==null;area=null
                                    } else if(tool!=="Stamp"&&tool!=="Add text")area={x:start.x,y:start.y,width:0,height:0}
                                }
                                onPositionChanged:function(mouse){
                                    let x=mouse.x/pageDelegate.scale,y=mouse.y/pageDelegate.scale
                                    stampPointer=Qt.point(window.snap(x,"x",modelData),window.snap(y,"y",modelData))
                                    if(pressed&&moving&&original){selectedObject=Object.assign({},original,{x:window.snap(original.x+x-start.x,"x",modelData),y:window.snap(original.y+y-start.y,"y",modelData)})}
                                    else if(pressed&&area)area={x:Math.min(start.x,x),y:Math.min(start.y,y),width:Math.abs(x-start.x),height:Math.abs(y-start.y)}
                                }
                                onReleased:function(mouse){
                                    if(pageDelegate.index!==doc.page)return
                                    let x=mouse.x/pageDelegate.scale,y=mouse.y/pageDelegate.scale
                                    if(moving&&original&&selectedObject&&(Math.abs(x-start.x)+Math.abs(y-start.y)>1))window.moveObject(selectedObject.x,selectedObject.y,selectedObject.width,selectedObject.height)
                                    else if(tool==="Stamp"&&stampArtwork!=="")documentController.placeStamp(stampArtwork,stampPointer.x*25.4/72,stampPointer.y*25.4/72,stampWidth,stampHeight,stampRotation,stampOpacity,stampFrame)
                                    else if(tool==="Add text"){newTextX.text=(x*25.4/72).toFixed(2);newTextY.text=(y*25.4/72).toFixed(2);addTextDialog.open()}
                                    else if(tool==="Annotate"&&area&&area.width>1&&area.height>1)documentController.annotate(annotationChoice.currentText,area.x,area.y,area.width,area.height,noteField.text)
                                    moving=false;original=null
                                }
                                Keys.onPressed:function(event){
                                    if(!selectedObject||selectionKind==="Text")return
                                    let amount=(event.modifiers&Qt.ShiftModifier?5:.5)*72/25.4
                                    if(event.key===Qt.Key_Left){window.nudge(-amount,0);event.accepted=true}
                                    if(event.key===Qt.Key_Right){window.nudge(amount,0);event.accepted=true}
                                    if(event.key===Qt.Key_Up){window.nudge(0,-amount);event.accepted=true}
                                    if(event.key===Qt.Key_Down){window.nudge(0,amount);event.accepted=true}
                                    if(event.key===Qt.Key_Delete){if(selectionKind==="Image")documentController.deleteImage(selectedObject.id);else documentController.deleteAnnotation(selectedObject.id);event.accepted=true}
                                }
                            }
                            PinchHandler {target:null;property real initialZoom:1;onActiveChanged:if(active)initialZoom=pageDelegate.scale;onActiveScaleChanged:if(active){zoomMode="Custom";zoom=Math.max(.1,Math.min(4,initialZoom*activeScale))}}
                        }
                        Label {anchors.bottom:parent.bottom;anchors.horizontalCenter:parent.horizontalCenter;text:"Page "+(index+1);font.pixelSize:11}
                    }
                    ScrollBar.vertical:ScrollBar {}
                }
                ColumnLayout {anchors.centerIn:parent;visible:!documentController.hasDocument;width:Math.min(parent.width-32,420);spacing:18
                    Label {text:"yolopdf";font.pixelSize:38;font.bold:true;Layout.alignment:Qt.AlignHCenter}
                    Label {text:"Edit text, work with images, apply stamps, and organize pages.";wrapMode:Text.Wrap;horizontalAlignment:Text.AlignHCenter;Layout.fillWidth:true}
                    Button {text:"Open document";Accessible.name:text;Layout.alignment:Qt.AlignHCenter;implicitHeight:48;onClicked:openDialog.open()}
                    Button {text:"Try sample";Accessible.name:text;Layout.alignment:Qt.AlignHCenter;implicitHeight:48;onClicked:documentController.openSample()}
                    Label {text:"Everything stays on your device.";Layout.alignment:Qt.AlignHCenter}
                    Repeater {model:documentController.recoveryCandidates
                        delegate:RowLayout {required property var modelData;Label {text:"Recover "+modelData.title;elide:Text.ElideRight;Layout.fillWidth:true}
                            Button {text:"Restore";Accessible.name:"Restore "+modelData.title;onClicked:documentController.restoreRecovery(modelData.id)}
                            ToolButton {text:"×";Accessible.name:"Discard recovery "+modelData.title;onClicked:documentController.discardRecovery(modelData.id)}
                        }
                    }
                }
            }
            ScrollView {
                id:properties;Layout.preferredWidth:264;Layout.fillHeight:true;visible:!mobile&&documentController.hasDocument&&(selectedObject!==null||tool==="Stamp"||tool==="Image"||tool==="Annotate")
                ColumnLayout {
                    width:properties.width-20;spacing:10
                    Label {text:selectedObject?selectionKind+" properties":tool+" tool";font.bold:true;Layout.topMargin:12}
                    Label {visible:selectedObject&&selectionKind==="Text";text:selectedObject?selectedObject.font+" · "+selectedObject.status:"";wrapMode:Text.Wrap;Layout.fillWidth:true}
                    Label {visible:selectedObject&&selectionKind==="Text";text:selectedObject?selectedObject.reason:"";wrapMode:Text.Wrap;Layout.fillWidth:true}
                    GridLayout {columns:2;visible:selectedObject!==null&&selectionKind!=="Text";Layout.fillWidth:true
                        Label {text:"X (mm)"} TextField {id:objectX;text:selectedObject?(selectedObject.x*25.4/72).toFixed(2):"0";Accessible.name:"Object X in millimetres";Layout.fillWidth:true}
                        Label {text:"Y (mm)"} TextField {id:objectY;text:selectedObject?(selectedObject.y*25.4/72).toFixed(2):"0";Accessible.name:"Object Y in millimetres";Layout.fillWidth:true}
                        Label {text:"W (mm)"} TextField {id:objectW;text:selectedObject?(selectedObject.width*25.4/72).toFixed(2):"0";enabled:selectionKind==="Image";Accessible.name:"Object width in millimetres";Layout.fillWidth:true}
                        Label {text:"H (mm)"} TextField {id:objectH;text:selectedObject?(selectedObject.height*25.4/72).toFixed(2):"0";enabled:selectionKind==="Image";Accessible.name:"Object height in millimetres";Layout.fillWidth:true}
                    }
                    Button {text:"Apply position / size";visible:selectedObject!==null&&selectionKind!=="Text";Accessible.name:text;enabled:window.enabledEdit();onClicked:window.moveObject(parseFloat(objectX.text)*72/25.4,parseFloat(objectY.text)*72/25.4,parseFloat(objectW.text)*72/25.4,parseFloat(objectH.text)*72/25.4)}
                    RowLayout {visible:selectedObject!==null&&selectionKind==="Image"
                        Button {text:"Copy";Accessible.name:"Copy image";onClicked:documentController.copyImage(selectedObject.id)}
                        Button {text:"Export";Accessible.name:"Export image";onClicked:exportImageDialog.open()}
                    }
                    Button {text:"Replace image…";visible:selectedObject!==null&&selectionKind==="Image";Accessible.name:text;enabled:window.enabledEdit();onClicked:{imagePurpose="replace";imageDialog.open()}}
                    Button {text:"Delete object";visible:selectedObject!==null&&selectionKind!=="Text";Accessible.name:text;enabled:window.enabledEdit();onClicked:{if(selectionKind==="Image")documentController.deleteImage(selectedObject.id);else documentController.deleteAnnotation(selectedObject.id)}}
                    RowLayout {visible:!!selectedObject&&selectionKind==="Annotation"&&selectedObject.type==="Stamp"
                        Button {text:"Rotate 90°";Accessible.name:text;onClicked:documentController.rotateStamp(selectedObject.id,90)}
                        Button {text:"Duplicate";Accessible.name:"Duplicate stamp";onClicked:documentController.duplicateStamp(selectedObject.id)}
                    }
                    Slider {visible:selectedObject!==null&&selectionKind==="Annotation";from:0;to:1;value:selectedObject?selectedObject.opacity:1;Accessible.name:"Annotation opacity";onMoved:if(!documentController.busy)documentController.annotationOpacity(selectedObject.id,value)}
                    CheckBox {text:"Snap to guides";checked:snapping;Accessible.name:text;onToggled:snapping=checked}
                    ComboBox {id:imageChoice;visible:tool==="Image";model:["Select image object","Select area"];Accessible.name:"Image selection mode"}
                    Button {text:"Insert image…";visible:tool==="Image";Accessible.name:text;enabled:window.enabledEdit();onClicked:{imagePurpose="insert";imageDialog.open()}}
                    Button {text:"Copy selected area";visible:area!==null;Accessible.name:text;enabled:!!doc.mayCopy&&!documentController.busy;onClicked:{if(area)documentController.copyArea(area.x,area.y,area.width,area.height)}}
                    Button {text:"Copy selected text";visible:area!==null;Accessible.name:text;enabled:!!doc.mayCopy&&!documentController.busy;onClicked:{if(area)documentController.copySelection(area.x,area.y,area.x+area.width,area.y+area.height)}}
                    Button {text:"Paste image";visible:tool==="Image";Accessible.name:text;enabled:window.enabledEdit();onClicked:documentController.pasteImage(20,20,100,60)}
                    Button {text:"Stamp collection…";visible:tool==="Stamp";Accessible.name:text;onClicked:collectionDialog.open()}
                    GridLayout {columns:2;visible:tool==="Stamp"
                        Label {text:"Width (mm)"} SpinBox {from:1;to:500;value:stampWidth;Accessible.name:"Stamp width in millimetres";onValueModified:stampWidth=value}
                        Label {text:"Height (mm)"} SpinBox {from:1;to:500;value:stampHeight;Accessible.name:"Stamp height in millimetres";onValueModified:stampHeight=value}
                        Label {text:"Rotation"} SpinBox {from:-180;to:180;value:stampRotation;Accessible.name:"Stamp rotation";onValueModified:stampRotation=value}
                        Label {text:"GIF frame"} SpinBox {from:0;to:10000;value:stampFrame;Accessible.name:"GIF frame";onValueModified:stampFrame=value}
                    }
                    Button {text:"Apply to selected pages";visible:tool==="Stamp";Accessible.name:text;enabled:window.enabledEdit()&&stampArtwork!=="";onClicked:documentController.placeStamp(stampArtwork,10,10,stampWidth,stampHeight,stampRotation,stampOpacity,stampFrame,selectedPages)}
                    Button {text:"Done";visible:tool==="Stamp";Accessible.name:text;onClicked:tool="Select"}
                    ComboBox {id:annotationChoice;visible:tool==="Annotate";model:["Highlight","Note","Arrow","Rectangle"];Accessible.name:"Annotation type"}
                    TextField {id:noteField;visible:tool==="Annotate";placeholderText:"Comment";Accessible.name:"Annotation comment";Layout.fillWidth:true}
                    Label {visible:tool==="Annotate";text:"Drag on the page to annotate.";wrapMode:Text.Wrap;Layout.fillWidth:true}
                }
            }
        }
        RowLayout {Layout.fillWidth:true;Layout.preferredHeight:32
            BusyIndicator {running:documentController.busy;visible:running;Layout.preferredWidth:28;Layout.preferredHeight:28;Accessible.name:"Working on document"}
            Label {Layout.fillWidth:true;text:documentController.busy?"Working…":documentController.hasDocument?(doc.noTextReason||((doc.page+1)+" / "+pages.length)) : "Ready";elide:Text.ElideRight}
            Button {visible:mobile&&documentController.hasDocument;text:"Properties";Accessible.name:text;onClicked:mobileProperties.open()}
            Label {text:doc.encrypted?"Encrypted":""} Label {text:doc.signed?"Signed":""}
        }
    }
    Popup {
        id:textPopup;width:Math.min(560,window.width-16);modal:false;closePolicy:Popup.CloseOnEscape
        property var run:({});property url fontFile:"";property int face:0;property bool sizeChanged:false
        onClosed:{previewTimer.stop();documentController.cancelPreview()}
        contentItem:ColumnLayout {spacing:8
            Label {text:textPopup.run.font||"Edit text";font.bold:true;Layout.fillWidth:true;elide:Text.ElideMiddle}
            Label {text:textPopup.fontFile!==""?"Substitute font (whole line)":"Original fonts and styles"}
            TextField {id:editor;Layout.fillWidth:true;Accessible.name:"Replacement text";font.family:(textPopup.run.font||"").replace(/^[A-Z]{6}\+/,"");font.pixelSize:18;onTextEdited:previewTimer.restart()}
            RowLayout {
                Label {text:"Size (pt)"}
                TextField {id:editSize;Layout.preferredWidth:80;validator:DoubleValidator {bottom:1;top:1200} Accessible.name:"Font size in points";onTextEdited:{textPopup.sizeChanged=true;previewTimer.restart()}}
                Button {text:"Find installed match";Accessible.name:text;onClicked:documentController.findInstalledFont(textPopup.run.font)}
                Button {text:"Import font…";Accessible.name:text;onClicked:{fontPurpose="edit";editFontDialog.open()}}
            }
            TextField {id:editColor;placeholderText:"Original colours · type #RRGGBB to change";Layout.fillWidth:true;Accessible.name:"Text colour";onTextEdited:previewTimer.restart()}
            SpinBox {from:0;to:255;value:textPopup.face;Accessible.name:"Font collection face index";onValueModified:{textPopup.face=value;previewTimer.restart()}}
            CheckBox {id:allowWider;text:"Allow wider text (may overlap neighbours)";Accessible.name:text;onToggled:{if(checked)wrapText.checked=false;previewTimer.restart()}}
            CheckBox {id:wrapText;text:"Wrap into empty space";Accessible.name:text;onToggled:{if(checked)allowWider.checked=false;previewTimer.restart()}}
            TextField {id:wrapWidthMm;visible:wrapText.checked;placeholderText:"Wrap width (mm)";Accessible.name:"Wrap width in millimetres";onTextEdited:previewTimer.restart()}
            RowLayout {Layout.alignment:Qt.AlignRight
                Button {text:"Cancel";Accessible.name:text;onClicked:textPopup.close()}
                Button {text:"Apply";Accessible.name:text;enabled:!documentController.busy&&(textPopup.run.status==="Original"||textPopup.fontFile!=="");onClicked:{documentController.replaceText(textPopup.run.id,editor.text,textPopup.fontFile,textPopup.sizeChanged?parseFloat(editSize.text):0,allowWider.checked,textPopup.face,editColor.text,wrapText.checked?parseFloat(wrapWidthMm.text)*72/25.4:0);textPopup.close()}}
            }
        }
    }
    Timer {id:previewTimer;interval:150;onTriggered:if(textPopup.opened)documentController.previewText(textPopup.run.id,editor.text,textPopup.fontFile,textPopup.sizeChanged?parseFloat(editSize.text):0,allowWider.checked,textPopup.face,editColor.text,wrapText.checked?parseFloat(wrapWidthMm.text)*72/25.4:0)}
    Drawer {id:mobileProperties;edge:Qt.BottomEdge;width:window.width;height:Math.min(420,window.height*.65)
        ColumnLayout {anchors.fill:parent;anchors.margins:16
            Label {text:selectedObject?selectionKind+" selected":tool; font.bold:true}
            Button {text:"Select image object";Accessible.name:text;onClicked:{tool="Image";imageChoice.currentIndex=0;mobileProperties.close()}}
            Button {text:"Select area";Accessible.name:text;onClicked:{tool="Image";imageChoice.currentIndex=1;mobileProperties.close()}}
            Button {text:"Insert image…";Accessible.name:text;onClicked:{imagePurpose="insert";imageDialog.open()}}
            Button {text:"Choose stamp…";Accessible.name:text;onClicked:stampDialog.open()}
            Button {text:"Copy image";visible:selectedObject!==null&&selectionKind==="Image";Accessible.name:text;onClicked:documentController.copyImage(selectedObject.id)}
            Button {text:"Copy area";visible:area!==null;Accessible.name:text;onClicked:documentController.copyArea(area.x,area.y,area.width,area.height)}
            Button {text:"Delete object";visible:selectedObject!==null&&selectionKind!=="Text";Accessible.name:text;onClicked:{if(selectionKind==="Image")documentController.deleteImage(selectedObject.id);else documentController.deleteAnnotation(selectedObject.id)}}
            Button {text:"Done";Accessible.name:text;onClicked:{tool="Select";mobileProperties.close()}}
            Item {Layout.fillHeight:true}
        }
    }
    FileDialog {id:openDialog;title:"Open PDF";nameFilters:["PDF documents (*.pdf)"];fileMode:FileDialog.OpenFiles;onAccepted:documentController.dropFiles(selectedFiles,false)}
    FileDialog {id:saveDialog;title:"Save PDF As";nameFilters:["PDF documents (*.pdf)"];defaultSuffix:"pdf";fileMode:FileDialog.SaveFile;onAccepted:documentController.saveAs(selectedFile)}
    FileDialog {id:insertDialog;title:"Insert pages";nameFilters:["PDF documents (*.pdf)"];fileMode:FileDialog.OpenFiles;onAccepted:{insertSettings.sources=selectedFiles;insertSettings.open()}}
    FileDialog {id:extractDialog;title:"Extract selected pages";defaultSuffix:"pdf";nameFilters:["PDF documents (*.pdf)"];fileMode:FileDialog.SaveFile;onAccepted:documentController.extractPages(selectedPages,selectedFile)}
    FileDialog {id:exportImageDialog;title:"Export image";defaultSuffix:"png";nameFilters:["PNG image (*.png)"];fileMode:FileDialog.SaveFile;onAccepted:documentController.exportImage(selectedObject.id,selectedFile)}
    FileDialog {id:imageDialog;title:imagePurpose==="replace"?"Replace image":"Insert image";nameFilters:["Images (*.png *.jpg *.jpeg *.gif)"];onAccepted:{if(imagePurpose==="replace")documentController.replaceImage(selectedObject.id,selectedFile);else documentController.insertImage(selectedFile,20,20,100,60)}}
    FileDialog {id:stampDialog;title:"Choose stamp artwork";nameFilters:["Stamp artwork (*.png *.svg *.gif *.pdf)"];onAccepted:{stampArtwork=selectedFile;documentController.previewStamp(stampArtwork,stampWidth,stampHeight,stampFrame);stampSettings.open()}}
    FileDialog {id:editFontDialog;title:"Import font for this run";nameFilters:["Fonts (*.ttf *.otf *.ttc)"];onAccepted:assetLibrary.importFont(selectedFile)}
    FileDialog {id:addFontDialog;title:"Choose font for new text";nameFilters:["Fonts (*.ttf *.otf *.ttc)"];onAccepted:{fontPurpose="add";assetLibrary.importFont(selectedFile)}}
    Dialog {id:printSettings;title:"Print";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel
        ColumnLayout {RadioButton {id:actualPrint;text:"Actual size";checked:true;Accessible.name:text}RadioButton {text:"Fit to paper";Accessible.name:text}}
        onAccepted:documentController.printDocument(!actualPrint.checked)
    }
    Dialog {id:passwordDialog;title:"PDF password";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel
        TextField {id:passwordField;echoMode:TextInput.Password;Accessible.name:"PDF password";onAccepted:passwordDialog.accept()}
        onAccepted:{documentController.providePassword(passwordField.text);passwordField.clear()}
        onRejected:passwordField.clear()
    }
    Dialog {id:signatureDialog;title:"Edit a signed PDF?";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel
        Label {text:"Editing will make the signature show the document as modified. Existing signed bytes are preserved through incremental saves.";width:Math.min(420,window.width-64);wrapMode:Text.Wrap}
        onAccepted:documentController.authorizeSignedEdits(true);onRejected:documentController.authorizeSignedEdits(false)
    }
    Dialog {id:closeDialog;title:"Unsaved changes";anchors.centerIn:Overlay.overlay;modal:true
        ColumnLayout {Label {text:"Save this document before closing?"}
            RowLayout {
                Button {text:"Cancel";Accessible.name:text;onClicked:closeDialog.close()}
                Button {text:"Discard";Accessible.name:text;onClicked:{documentController.closeTab(tabToClose,true);closeDialog.close()}}
                Button {text:"Save";Accessible.name:text;onClicked:{closeAfterSave=true;saveDialog.open();closeDialog.close()}}
            }
        }
    }
    Dialog {id:quitDialog;title:"Unsaved documents";anchors.centerIn:Overlay.overlay;modal:true
        ColumnLayout {Label {text:"Unsaved work can be kept for recovery on your next launch.";width:Math.min(380,window.width-64);wrapMode:Text.Wrap}
            RowLayout {Button {text:"Cancel";Accessible.name:text;onClicked:quitDialog.close()}
                Button {text:"Keep recovery and quit";Accessible.name:text;onClicked:{allowQuit=true;Qt.quit()}}
            }
        }
    }
    Dialog {id:insertSettings;title:"Insert pages";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel;property var sources:[]
        ColumnLayout {TextField {id:insertRange;placeholderText:"All pages, or 1-3,7";Accessible.name:"Page range"}
            SpinBox {id:insertAt;from:1;to:pages.length+1;value:Math.min(doc.page+2,pages.length+1);Accessible.name:"Insertion position"}}
        onAccepted:documentController.insertPages(sources,insertAt.value-1,insertRange.text)
    }
    Dialog {id:blankDialog;title:"Blank page";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel
        ColumnLayout {ComboBox {id:blankSize;model:["Current page","A4","Letter","A5","Custom"];Accessible.name:"Blank page size";onActivated:{if(currentIndex===1){blankWidth.text="210";blankHeight.text="297"}else if(currentIndex===2){blankWidth.text="215.9";blankHeight.text="279.4"}else if(currentIndex===3){blankWidth.text="148";blankHeight.text="210"}}}
            TextField {id:blankWidth;text:pages.length?(pages[doc.page].width*25.4/72).toFixed(2):"210";Accessible.name:"Blank page width in millimetres"}
            TextField {id:blankHeight;text:pages.length?(pages[doc.page].height*25.4/72).toFixed(2):"297";Accessible.name:"Blank page height in millimetres"}}
        onAccepted:documentController.blankPage(doc.page+1,parseFloat(blankWidth.text),parseFloat(blankHeight.text))
    }
    Dialog {id:transferDialog;title:"Move or copy selected pages";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel
        ColumnLayout {ComboBox {id:targetTab;model:documentController.tabs.filter(t=>t.id!==doc.id);textRole:"title";Accessible.name:"Destination document"}
            CheckBox {id:moveSelection;text:"Move pages";Accessible.name:text}
            SpinBox {id:transferAt;from:1;to:100000;value:1;Accessible.name:"Destination insertion position"}}
        onAccepted:documentController.transferPages(targetTab.model[targetTab.currentIndex].id,selectedPages,transferAt.value-1,moveSelection.checked)
    }
    Dialog {id:stampSettings;title:"Create stamp";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel;property real ratio:stampHeight/stampWidth
        ColumnLayout {Image {source:documentController.stampPreviewSource;Layout.preferredWidth:240;Layout.preferredHeight:140;fillMode:Image.PreserveAspectFit;Accessible.name:"Stamp artwork preview"}
            TextField {id:stampWidthField;text:String(stampWidth);placeholderText:"Width (mm)";Accessible.name:"Stamp width in millimetres";onTextEdited:{if(stampAspect.checked)stampHeightField.text=String(parseFloat(text)*stampSettings.ratio);stampPreviewTimer.restart()}}
            TextField {id:stampHeightField;text:String(stampHeight);placeholderText:"Height (mm)";Accessible.name:"Stamp height in millimetres";onTextEdited:stampPreviewTimer.restart()}
            CheckBox {id:stampAspect;text:"Lock aspect ratio";checked:true;Accessible.name:text}
            TextField {id:stampName;placeholderText:"Stamp name";Accessible.name:"Stamp name"}
            SpinBox {id:createGifFrame;from:0;to:10000;value:stampFrame;Accessible.name:"GIF frame";onValueModified:stampPreviewTimer.restart()}
            Label {text:"The placement tool stays active until Done or Esc.";wrapMode:Text.Wrap;Layout.maximumWidth:280}}
        onAccepted:assetLibrary.importStamp(stampArtwork,stampName.text,parseFloat(stampWidthField.text),parseFloat(stampHeightField.text),stampAspect.checked,createGifFrame.value)
    }
    Timer {id:stampPreviewTimer;interval:150;onTriggered:documentController.previewStamp(stampArtwork,parseFloat(stampWidthField.text),parseFloat(stampHeightField.text),createGifFrame.value)}
    FileDialog {id:collectionExport;title:"Export stamp collection";fileMode:FileDialog.SaveFile;defaultSuffix:"yolostamps";onAccepted:assetLibrary.exportStamps(selectedFile)}
    FileDialog {id:collectionImport;title:"Import stamp collection";nameFilters:["Stamp collection (*.yolostamps)"];onAccepted:assetLibrary.importStamps(selectedFile)}
    Dialog {id:collectionDialog;title:"Stamp collection";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Close;width:Math.min(460,window.width-24)
        contentItem:ColumnLayout {
            ListView {Layout.fillWidth:true;Layout.preferredHeight:260;model:assetLibrary.stamps;clip:true;spacing:6
                delegate:RowLayout {required property var modelData;width:ListView.view.width
                    Button {text:modelData.name+" · "+modelData.widthMm+" × "+modelData.heightMm+" mm";Accessible.name:text;Layout.fillWidth:true;enabled:!assetLibrary.busy
                        onClicked:{stampArtwork=modelData.file;stampWidth=modelData.widthMm;stampHeight=modelData.heightMm;stampFrame=modelData.gifFrame;tool="Stamp";collectionDialog.close()}}
                    ToolButton {text:"×";Accessible.name:"Delete "+modelData.name;enabled:!assetLibrary.busy&&!documentController.busy;onClicked:assetLibrary.remove("stamps",modelData.id)}
                }
            }
            RowLayout {Button {text:"New…";Accessible.name:"Create stamp";enabled:!assetLibrary.busy;onClicked:{collectionDialog.close();stampDialog.open()}}
                Button {text:"Import…";Accessible.name:"Import stamp collection";enabled:!assetLibrary.busy;onClicked:collectionImport.open()}
                Button {text:"Export…";Accessible.name:"Export stamp collection";enabled:!assetLibrary.busy;onClicked:collectionExport.open()}}
            BusyIndicator {running:assetLibrary.busy;visible:running;Accessible.name:"Saving stamp collection"}
        }
    }
    Dialog {id:addTextDialog;title:"Add text";anchors.centerIn:Overlay.overlay;modal:true;standardButtons:Dialog.Ok|Dialog.Cancel
        ColumnLayout {TextArea {id:newText;Layout.preferredWidth:Math.min(400,window.width-64);Layout.preferredHeight:100;placeholderText:"Text";Accessible.name:"New text"}
            Button {text:"Choose font…";Accessible.name:text;onClicked:addFontDialog.open()}
            GridLayout {columns:2
                Label {text:"X (mm)"} TextField {id:newTextX;Accessible.name:"New text X in millimetres"}
                Label {text:"Y baseline (mm)"} TextField {id:newTextY;Accessible.name:"New text baseline in millimetres"}
                Label {text:"Size (pt)"} TextField {id:newTextSize;text:"12";Accessible.name:"New text font size"}
                Label {text:"Width (mm, 0 = point text)"} TextField {id:newTextWidth;text:"0";Accessible.name:"New text box width"}
                Label {text:"Height (mm, 0 = unlimited)"} TextField {id:newTextHeight;text:"0";Accessible.name:"New text box height"}
                Label {text:"Colour"} TextField {id:newTextColor;text:"#000000";Accessible.name:"New text colour"}
            }
            ComboBox {model:assetLibrary.fonts;textRole:"name";Accessible.name:"Imported font";onActivated:textFont=model[currentIndex].file}
            ComboBox {id:newTextAlignment;model:["Left","Centre","Right"];Accessible.name:"Text alignment"}}
        onAccepted:documentController.addText(newText.text,textFont,parseFloat(newTextX.text)*72/25.4,parseFloat(newTextY.text)*72/25.4,parseFloat(newTextWidth.text)*72/25.4,parseFloat(newTextHeight.text)*72/25.4,parseFloat(newTextSize.text),newTextColor.text,newTextAlignment.currentIndex)
    }
}
