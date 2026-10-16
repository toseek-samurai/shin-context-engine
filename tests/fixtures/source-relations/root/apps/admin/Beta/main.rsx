<App>
  <JavascriptQuery id="PageInit" query={include("./lib/PageInit.js", "string")} />
  <State id="OtherAppOnly" value={1} />
  <Text id="Result" value="{{ PageInit.data }}" />
</App>
