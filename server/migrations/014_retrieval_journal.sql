CREATE TABLE shin_server.retrieval_journal_producers (
    instance_id TEXT NOT NULL,
    source_host_id TEXT NOT NULL,
    server_version TEXT NOT NULL,
    core_version TEXT NOT NULL,
    observed_through BIGINT NOT NULL DEFAULT 0,
    captured_count BIGINT NOT NULL DEFAULT 0,
    missing_count BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT retrieval_journal_producers_pkey PRIMARY KEY (instance_id),
    CONSTRAINT retrieval_journal_producers_instance_check CHECK (instance_id ~ '^[0-9a-f]{64}$'),
    CONSTRAINT retrieval_journal_producers_source_host_check CHECK (source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT retrieval_journal_producers_server_version_check CHECK (octet_length(server_version) BETWEEN 1 AND 128),
    CONSTRAINT retrieval_journal_producers_core_version_check CHECK (octet_length(core_version) BETWEEN 1 AND 128),
    CONSTRAINT retrieval_journal_producers_progress_check CHECK (
        observed_through >= 0 AND captured_count >= 0 AND missing_count >= 0 AND
        captured_count + missing_count = observed_through
    )
);

CREATE TABLE shin_server.retrieval_journal_attempts (
    id BIGINT GENERATED ALWAYS AS IDENTITY,
    instance_id TEXT NOT NULL,
    sequence BIGINT NOT NULL,
    record_hash TEXT NOT NULL,
    project_id TEXT NOT NULL,
    operation TEXT NOT NULL,
    selector_kind TEXT NOT NULL,
    selector_id TEXT NOT NULL,
    observed_at_unix_ms BIGINT NOT NULL,
    status INTEGER NOT NULL,
    reason_code TEXT,
    last_phase TEXT NOT NULL,
    elapsed_ns BIGINT NOT NULL,
    result_bytes BIGINT,
    validation_ns BIGINT NOT NULL,
    selection_ns BIGINT,
    admission_ns BIGINT,
    execution_ns BIGINT,
    finalization_ns BIGINT,
    route_id TEXT,
    workspace_id TEXT,
    source_host_id TEXT,
    snapshot_id TEXT,
    preparation_id BIGINT,
    revision BIGINT,
    profile_id TEXT,
    profile_fingerprint TEXT,
    route_fingerprint TEXT,
    retrieval_mode TEXT,
    returned_records BIGINT,
    candidate_rows BIGINT,
    covered_groups BIGINT,
    total_groups BIGINT,
    pack_complete BOOLEAN,
    pack_coverage_complete BOOLEAN,
    required_requirements BIGINT,
    satisfied_required_requirements BIGINT,
    query_capture TEXT NOT NULL,
    request_fingerprint TEXT,
    request JSONB,
    received_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT retrieval_journal_attempts_pkey PRIMARY KEY (id),
    CONSTRAINT retrieval_journal_attempts_instance_sequence_unique UNIQUE (instance_id, sequence),
    CONSTRAINT retrieval_journal_attempts_instance_fk FOREIGN KEY (instance_id)
        REFERENCES shin_server.retrieval_journal_producers(instance_id) ON DELETE RESTRICT,
    CONSTRAINT retrieval_journal_attempts_project_fk FOREIGN KEY (project_id)
        REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    CONSTRAINT retrieval_journal_attempts_id_check CHECK (id > 0),
    CONSTRAINT retrieval_journal_attempts_sequence_check CHECK (sequence > 0),
    CONSTRAINT retrieval_journal_attempts_record_hash_check CHECK (record_hash ~ '^[0-9a-f]{64}$'),
    CONSTRAINT retrieval_journal_attempts_operation_check CHECK (
        operation = 'search' OR operation = 'brief' OR operation = 'pack'
    ),
    CONSTRAINT retrieval_journal_attempts_selector_kind_check CHECK (
        selector_kind = 'route' OR selector_kind = 'publication'
    ),
    CONSTRAINT retrieval_journal_attempts_selector_id_check CHECK (selector_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT retrieval_journal_attempts_observed_at_check CHECK (observed_at_unix_ms >= 0),
    CONSTRAINT retrieval_journal_attempts_status_check CHECK (status BETWEEN 100 AND 599),
    CONSTRAINT retrieval_journal_attempts_reason_check CHECK (
        (status = 200 AND reason_code IS NULL) OR
        (status <> 200 AND reason_code IS NOT NULL AND reason_code ~ '^[a-z][a-z0-9-]{0,63}$')
    ),
    CONSTRAINT retrieval_journal_attempts_last_phase_check CHECK (
        last_phase = 'validation' OR last_phase = 'selection' OR last_phase = 'admission' OR
        last_phase = 'execution' OR last_phase = 'finalization'
    ),
    CONSTRAINT retrieval_journal_attempts_elapsed_check CHECK (elapsed_ns >= 0),
    CONSTRAINT retrieval_journal_attempts_result_bytes_check CHECK (result_bytes IS NULL OR result_bytes >= 0),
    CONSTRAINT retrieval_journal_attempts_validation_ns_check CHECK (validation_ns >= 0),
    CONSTRAINT retrieval_journal_attempts_selection_ns_check CHECK (selection_ns IS NULL OR selection_ns >= 0),
    CONSTRAINT retrieval_journal_attempts_admission_ns_check CHECK (admission_ns IS NULL OR admission_ns >= 0),
    CONSTRAINT retrieval_journal_attempts_execution_ns_check CHECK (execution_ns IS NULL OR execution_ns >= 0),
    CONSTRAINT retrieval_journal_attempts_finalization_ns_check CHECK (finalization_ns IS NULL OR finalization_ns >= 0),
    CONSTRAINT retrieval_journal_attempts_phase_presence_check CHECK (
        (last_phase = 'validation' AND selection_ns IS NULL AND admission_ns IS NULL AND execution_ns IS NULL AND finalization_ns IS NULL) OR
        (last_phase = 'selection' AND selection_ns IS NOT NULL AND admission_ns IS NULL AND execution_ns IS NULL AND finalization_ns IS NULL) OR
        (last_phase = 'admission' AND selection_ns IS NOT NULL AND admission_ns IS NOT NULL AND execution_ns IS NULL AND finalization_ns IS NULL) OR
        (last_phase = 'execution' AND selection_ns IS NOT NULL AND admission_ns IS NOT NULL AND execution_ns IS NOT NULL AND finalization_ns IS NULL) OR
        (last_phase = 'finalization' AND selection_ns IS NOT NULL AND admission_ns IS NOT NULL AND execution_ns IS NOT NULL AND finalization_ns IS NOT NULL)
    ),
    CONSTRAINT retrieval_journal_attempts_phase_sum_check CHECK (
        elapsed_ns = validation_ns + COALESCE(selection_ns, 0) + COALESCE(admission_ns, 0) +
            COALESCE(execution_ns, 0) + COALESCE(finalization_ns, 0)
    ),
    CONSTRAINT retrieval_journal_attempts_route_id_check CHECK (route_id IS NULL OR route_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT retrieval_journal_attempts_workspace_id_check CHECK (workspace_id IS NULL OR workspace_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT retrieval_journal_attempts_source_host_id_check CHECK (source_host_id IS NULL OR source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT retrieval_journal_attempts_snapshot_id_check CHECK (snapshot_id IS NULL OR snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$'),
    CONSTRAINT retrieval_journal_attempts_preparation_id_check CHECK (preparation_id IS NULL OR preparation_id >= 0),
    CONSTRAINT retrieval_journal_attempts_revision_check CHECK (revision IS NULL OR revision >= 0),
    CONSTRAINT retrieval_journal_attempts_profile_id_check CHECK (profile_id IS NULL OR profile_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT retrieval_journal_attempts_profile_fingerprint_check CHECK (
        profile_fingerprint IS NULL OR profile_fingerprint ~ '^fnv1a64:[0-9a-f]{16}$'
    ),
    CONSTRAINT retrieval_journal_attempts_route_fingerprint_check CHECK (
        route_fingerprint IS NULL OR route_fingerprint ~ '^[0-9a-f]{64}$'
    ),
    CONSTRAINT retrieval_journal_attempts_retrieval_mode_check CHECK (
        retrieval_mode IS NULL OR retrieval_mode = 'strict' OR retrieval_mode = 'relaxed' OR
        retrieval_mode = 'owner-scoped' OR retrieval_mode = 'directory-scoped' OR retrieval_mode = 'planned'
    ),
    CONSTRAINT retrieval_journal_attempts_returned_records_check CHECK (returned_records IS NULL OR returned_records >= 0),
    CONSTRAINT retrieval_journal_attempts_candidate_rows_check CHECK (candidate_rows IS NULL OR candidate_rows >= 0),
    CONSTRAINT retrieval_journal_attempts_covered_groups_check CHECK (covered_groups IS NULL OR covered_groups >= 0),
    CONSTRAINT retrieval_journal_attempts_total_groups_check CHECK (total_groups IS NULL OR total_groups >= 0),
    CONSTRAINT retrieval_journal_attempts_required_requirements_check CHECK (
        required_requirements IS NULL OR required_requirements >= 0
    ),
    CONSTRAINT retrieval_journal_attempts_satisfied_check CHECK (
        satisfied_required_requirements IS NULL OR satisfied_required_requirements >= 0
    ),
    CONSTRAINT retrieval_journal_attempts_query_capture_check CHECK (
        query_capture = 'disabled' OR query_capture = 'invalid-request' OR query_capture = 'captured' OR
        query_capture = 'omitted-budget' OR query_capture = 'capture-failed'
    ),
    CONSTRAINT retrieval_journal_attempts_uncaptured_request_check CHECK (
        query_capture = 'captured' OR (request IS NULL AND request_fingerprint IS NULL)
    ),
    -- Native validation limits compact request JSON to 8192 bytes. This defensive JSONB rendering cap is separate.
    CONSTRAINT retrieval_journal_attempts_captured_request_check CHECK (
        query_capture <> 'captured' OR
        (request IS NOT NULL AND jsonb_typeof(request) = 'object' AND octet_length(request::text) <= 16384 AND
            request_fingerprint IS NOT NULL AND request_fingerprint ~ '^[0-9a-f]{64}$')
    )
);

CREATE INDEX retrieval_journal_attempts_project_id_id_idx
    ON shin_server.retrieval_journal_attempts(project_id, id);
