<Screen id="Home" title="Fixture Home" urlSlug="home">
  <RESTQuery
    id="RetoolLoadWidget"
    query="/widgets"
    resourceDisplayName="FixtureApi"
    resourceName="fixture-resource"
    type="GET"
  >
    <Event
      id="event1"
      event="success"
      method="trigger"
      pluginId="RetoolSaveWidget"
      type="datasource"
    />
    <Event
      id="event2"
      event="success"
      method="run"
      params={{ map: { src: "await RetoolRefresh.trigger();" } }}
      pluginId=""
      type="script"
    />
  </RESTQuery>
  <JavascriptQuery
    id="RetoolSaveWidget"
    query={include("../lib/RetoolSaveWidget.js", "string")}
    resourceName="JavascriptQuery"
  />
  <SqlQueryUnified
    id="RetoolFindWidget"
    query={include("../lib/RetoolFindWidget.sql", "string")}
    resourceDisplayName="FixtureDb"
    resourceName="fixture-db"
  />
  <Form
    id="RetoolForm"
    disabled="{{ current_user == null }}"
    readonlyMode="always"
  >
    <TextInput
      id="RetoolName"
      formDataKey="name"
      hidden="{{ false }}"
      label="Fixture Name"
    />
  </Form>
</Screen>
