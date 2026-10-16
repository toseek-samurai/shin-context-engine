await LoadReport.trigger();
const literal = "QuotedInJs.trigger()";
const template = `TemplateInJs.data`;
// LineCommentInJs.data
/* BlockCommentInJs.trigger()
   MultilineCommentInJs.data */
Duplicate.trigger();
Missing.trigger();
Request.trigger();
BrokenPayload.trigger();
Compound.trigger();
Traversal.trigger();
OtherAppOnly.trigger();
holder.NestedInJs.trigger();
const regex = /RegexInJs.data/;
StateValue.setValue(1);
PageInit /* a comment between tokens */ .data;
Included.trigger();
