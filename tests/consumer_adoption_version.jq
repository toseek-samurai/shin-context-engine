def release_version_parts:
  if type != "string" then null
  elif test("^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$")
  then split(".") | map(tonumber)
  else null end;

def minimum_version_check($id; $actual; $expected):
  ($actual | release_version_parts) as $actual_parts |
  ($expected | release_version_parts) as $expected_parts |
  {id: $id, actual: $actual, expected: $expected, comparator: ">=",
   ok: ($actual_parts != null and $expected_parts != null and
        $actual_parts >= $expected_parts)};
