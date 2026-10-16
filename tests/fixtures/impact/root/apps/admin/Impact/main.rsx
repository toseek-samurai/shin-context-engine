<App>
  <JavascriptQuery id="LoadPrice" query={include("./lib/LoadPrice.js", "string")} />
  <JavascriptQuery id="SmokeCheck" query={include("./tests/smoke.test.js", "string")} />
  <Text id="PriceLabel" value="{{ LoadPrice.data }}" />
</App>
