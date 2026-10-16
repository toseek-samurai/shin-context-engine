<Screen id="View">
  <Table id="UI" data="{{ LoadReport.data }}" />
  <Button id="Run">
    <Event
      pluginId={"LoadReport"}
      event="click" method="trigger"
    />
  </Button>
  <Text id="Result" value={
    PageInit.data.brandId +
    UI.selectedRow.id
  } />
  <Text id="Quoted" value={'QuotedInRsx.data'} />
  <Text id="Plain" value="PlainInRsx.data" />
  <Text id="Commented" value={
    /* CommentInRsx.data } */
    // LineCommentInRsx.trigger()
    false
  } />
  <Event type="script" params={{ map: { src: "ScriptString.trigger()" } }} />
  <Event pluginId={"Computed" + suffix} />
  <Event pluginId={chosenTarget} />
  <Text id="Template" value="{{ 'QuotedInTemplate.data' + PageInit.data.brandId }}" />
  <Text id="Nested" value={holder.NestedTarget.trigger()} />
  <Text id="Included" value={include("../lib/First.sql", "string")} />
</Screen>
