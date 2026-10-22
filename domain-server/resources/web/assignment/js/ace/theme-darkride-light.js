// The script editor's light theme, for the world server's light look: Darkride's light page colours, with code in
// its cyan and violet. Laid out like the twilight theme beside it (theme-twilight.js, from Ace).

ace.define('ace/theme/darkride-light', ['require', 'exports', 'module' , 'ace/lib/dom'], function(require, exports, module) {

exports.isDark = false;
exports.cssClass = "ace-darkride-light";
exports.cssText = ".ace-darkride-light .ace_gutter {\
background: #eef1f7;\
color: #8a91ab\
}\
.ace-darkride-light .ace_print-margin {\
width: 1px;\
background: #e3e8f4\
}\
.ace-darkride-light {\
background-color: #ffffff;\
color: #1c2136\
}\
.ace-darkride-light .ace_cursor {\
border-left: 2px solid #0080c0\
}\
.ace-darkride-light .ace_overwrite-cursors .ace_cursor {\
border-left: 0px;\
border-bottom: 1px solid #0080c0\
}\
.ace-darkride-light .ace_marker-layer .ace_selection {\
background: rgba(0, 175, 255, 0.18)\
}\
.ace-darkride-light.ace_multiselect .ace_selection.ace_start {\
box-shadow: 0 0 3px 0px #ffffff;\
border-radius: 2px\
}\
.ace-darkride-light .ace_marker-layer .ace_step {\
background: rgb(255, 236, 160)\
}\
.ace-darkride-light .ace_marker-layer .ace_bracket {\
margin: -1px 0 0 -1px;\
border: 1px solid rgba(28, 33, 54, 0.3)\
}\
.ace-darkride-light .ace_marker-layer .ace_active-line {\
background: rgba(0, 175, 255, 0.06)\
}\
.ace-darkride-light .ace_gutter-active-line {\
background-color: rgba(0, 175, 255, 0.08)\
}\
.ace-darkride-light .ace_marker-layer .ace_selected-word {\
border: 1px solid rgba(0, 175, 255, 0.35)\
}\
.ace-darkride-light .ace_invisible {\
color: rgba(28, 33, 54, 0.25)\
}\
.ace-darkride-light .ace_keyword,\
.ace-darkride-light .ace_meta {\
color: #7b2fd1\
}\
.ace-darkride-light .ace_constant,\
.ace-darkride-light .ace_constant.ace_character,\
.ace-darkride-light .ace_constant.ace_character.ace_escape,\
.ace-darkride-light .ace_constant.ace_other,\
.ace-darkride-light .ace_heading,\
.ace-darkride-light .ace_support.ace_constant {\
color: #c2410c\
}\
.ace-darkride-light .ace_invalid.ace_illegal {\
color: #ffffff;\
background-color: rgba(217, 54, 54, 0.85)\
}\
.ace-darkride-light .ace_invalid.ace_deprecated {\
text-decoration: underline;\
font-style: italic;\
color: #d93636\
}\
.ace-darkride-light .ace_support {\
color: #5b6382\
}\
.ace-darkride-light .ace_fold {\
background-color: #0080c0;\
border-color: #1c2136\
}\
.ace-darkride-light .ace_support.ace_function {\
color: #0080c0\
}\
.ace-darkride-light .ace_list,\
.ace-darkride-light .ace_storage {\
color: #7b2fd1\
}\
.ace-darkride-light .ace_entity.ace_name.ace_function,\
.ace-darkride-light .ace_meta.ace_tag {\
color: #0080c0\
}\
.ace-darkride-light .ace_string {\
color: #15803d\
}\
.ace-darkride-light .ace_string.ace_regexp {\
color: #b45309\
}\
.ace-darkride-light .ace_comment {\
font-style: italic;\
color: #8a91ab\
}\
.ace-darkride-light .ace_variable {\
color: #1d4ed8\
}\
.ace-darkride-light .ace_xml-pe {\
color: #8a91ab\
}\
.ace-darkride-light .ace_indent-guide {\
box-shadow: inset -1px 0 0 #e6e9f2\
}";

var dom = require("../lib/dom");
dom.importCssString(exports.cssText, exports.cssClass);
});
