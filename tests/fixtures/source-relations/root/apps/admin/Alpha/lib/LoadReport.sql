select record_id
from records
where brand_id = {{ PageInit.data.brandId }}
  and record_id = {{ UI.selectedRow.id }};
-- {{ SqlLineComment.data }}
/* {{ SqlBlockComment.trigger() }} */
select '{{ SqlQuoted.data }}', "{{ SqlIdentifier.data }}";
select $$ {{ SqlDollarQuoted.data }} $$;
select $body$ {{ SqlNamedQuote.data }} $body$;
/* outer /* inner */ {{ SqlNestedComment.data }} */
select PageInit.data;
