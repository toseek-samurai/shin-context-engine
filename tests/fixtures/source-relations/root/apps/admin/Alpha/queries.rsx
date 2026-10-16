<GlobalFunctions>
  <JavascriptQuery id="PageInit" query={include("./lib/PageInit.js", "string")} />
  <SqlQueryUnified
    id="LoadReport"
    query={include("./lib/LoadReport.sql", "string")}
  />
  <SqlQueryUnified id="Duplicate" query={include("./lib/First.sql", "string")} />
  <SqlQueryUnified id="Duplicate" query={include("./lib/Second.sql", "string")} />
  <RESTQuery id="Request"
    headers={include("./lib/headers.json", "json")}
    body={include("./lib/body.json", "json")}
  />
  <JavascriptQuery id="BrokenPayload" query={include("./lib/Missing.js", "string")} />
  <JavascriptQuery id="Compound"
    query={choose ? include("./lib/First.sql", "string") : include("./lib/Second.sql", "string")}
  />
  <JavascriptQuery id="Traversal" query={include("../../../../outside.js", "string")} />
  <JavascriptQuery id="QuotedOnly" query="include('./lib/Phantom.js', 'string')" />
  <JavascriptQuery id="DynamicInclude" query={include("./lib/" + name, "string")} />
  <JavascriptQuery id="DynamicMode" query={include("./lib/Phantom.js", mode)} />
  <JavascriptQuery id="CommentInclude" query={/* include("./lib/Phantom.js", "string") */ false} />
  <JavascriptQuery id={"Computed" + suffix} query={include("./lib/First.sql", "string")} />
  <State id="StateValue" value={1} />
  <RESTQuery id="RequestQuery" query={include("./lib/First.sql", "string")} />
  <SqlQueryUnified id="UnindexedPayload" query={include("./not-indexed.txt", "string")} />
  <JavascriptQuery id="CommentedInclude" query={/* before */ include(/* path */ "./lib/First.sql", "string") /* after */} />
  <JavascriptQuery id="MemberInclude" query={loader. /* comment */ include("./lib/Phantom.js", "string")} />
  <Function id="Derived" funcBody={include("./lib/First.sql", "string")} />
  <Function id="CompoundFunction" funcBody={flag ? include("./lib/First.sql", "string") : include("./lib/Second.sql", "string")} />
  <Function id="WrongFunctionAttribute" query={include("./lib/First.sql", "string")} />
  <Function id="DynamicFunction" funcBody={include("./lib/" + name, "string")} />
</GlobalFunctions>
