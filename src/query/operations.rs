pub const WIRE_VERSION: u16 = 1;
pub const OP_CORS_ORIGIN_ALLOWED: u32 = 1;
pub const OP_PORTAL_ROUTES_METADATA: u32 = 2;
pub const OP_TENANT_BY_SITE: u32 = 3;
pub const OP_TENANT_BY_DOMAIN: u32 = 4;
pub const OP_TENANT_DOMAINS: u32 = 5;
pub const OP_AGENTS_LIST: u32 = 6;
pub const OP_TENANTS_LIST: u32 = 7;
pub const OP_TEMPLATES_LIST: u32 = 8;
pub const OP_TEMPLATE_GET: u32 = 9;

pub const CACHE_READ_THROUGH: u8 = 1;
pub const CACHE_BYPASS: u8 = 2;
pub const CACHE_HIT: u8 = 1;
pub const CACHE_MISS: u8 = 2;
pub const CACHE_BYPASS_STATUS: u8 = 3;
pub const STATUS_OK: u8 = 0;
pub const STATUS_ERROR: u8 = 1;

pub const CORS_ORIGIN_ALLOWED_ACTION: &str = "Core.Query.CorsOrigin.Allowed";
pub const PORTAL_ROUTES_METADATA_ACTION: &str = "Core.Query.Portal.RoutesMetadata";
pub const TENANT_BY_SITE_ACTION: &str = "Core.Query.Tenant.BySite";
pub const TENANT_BY_DOMAIN_ACTION: &str = "Core.Query.Tenant.ByDomain";
pub const TENANT_DOMAINS_ACTION: &str = "Core.Query.Tenant.Domains";
pub const AGENTS_LIST_ACTION: &str = "Core.Query.Agents.List";
pub const TENANTS_LIST_ACTION: &str = "Core.Query.Tenants.List";
pub const TEMPLATES_LIST_ACTION: &str = "Core.Query.Templates.List";
pub const TEMPLATE_GET_ACTION: &str = "Core.Query.Template.Get";
pub const CORS_ORIGIN_ALLOWED_FUNCTION: &str = "Query.CorsOrigin.Allowed";
pub const PORTAL_ROUTES_METADATA_FUNCTION: &str = "Query.Portal.RoutesMetadata";
pub const TENANT_BY_SITE_FUNCTION: &str = "Query.Tenant.BySite";
pub const TENANT_BY_DOMAIN_FUNCTION: &str = "Query.Tenant.ByDomain";
pub const TENANT_DOMAINS_FUNCTION: &str = "Query.Tenant.Domains";
pub const AGENTS_LIST_FUNCTION: &str = "Query.Agents.List";
pub const TENANTS_LIST_FUNCTION: &str = "Query.Tenants.List";
pub const TEMPLATES_LIST_FUNCTION: &str = "Query.Templates.List";
pub const TEMPLATE_GET_FUNCTION: &str = "Query.Template.Get";
pub const CACHE_INVALIDATE_FUNCTION: &str = "Query.Cache.Invalidate";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Operation {
    pub id: u32,
    pub action: &'static str,
    pub function: &'static str,
    pub boolean_response: bool,
}

pub const OPERATIONS: [Operation; 9] = [
    Operation {
        id: OP_CORS_ORIGIN_ALLOWED,
        action: CORS_ORIGIN_ALLOWED_ACTION,
        function: CORS_ORIGIN_ALLOWED_FUNCTION,
        boolean_response: true,
    },
    Operation {
        id: OP_PORTAL_ROUTES_METADATA,
        action: PORTAL_ROUTES_METADATA_ACTION,
        function: PORTAL_ROUTES_METADATA_FUNCTION,
        boolean_response: false,
    },
    Operation {
        id: OP_TENANT_BY_SITE,
        action: TENANT_BY_SITE_ACTION,
        function: TENANT_BY_SITE_FUNCTION,
        boolean_response: false,
    },
    Operation {
        id: OP_TENANT_BY_DOMAIN,
        action: TENANT_BY_DOMAIN_ACTION,
        function: TENANT_BY_DOMAIN_FUNCTION,
        boolean_response: false,
    },
    Operation {
        id: OP_TENANT_DOMAINS,
        action: TENANT_DOMAINS_ACTION,
        function: TENANT_DOMAINS_FUNCTION,
        boolean_response: false,
    },
    Operation {
        id: OP_AGENTS_LIST,
        action: AGENTS_LIST_ACTION,
        function: AGENTS_LIST_FUNCTION,
        boolean_response: false,
    },
    Operation {
        id: OP_TENANTS_LIST,
        action: TENANTS_LIST_ACTION,
        function: TENANTS_LIST_FUNCTION,
        boolean_response: false,
    },
    Operation {
        id: OP_TEMPLATES_LIST,
        action: TEMPLATES_LIST_ACTION,
        function: TEMPLATES_LIST_FUNCTION,
        boolean_response: false,
    },
    Operation {
        id: OP_TEMPLATE_GET,
        action: TEMPLATE_GET_ACTION,
        function: TEMPLATE_GET_FUNCTION,
        boolean_response: false,
    },
];

pub fn operation_by_action(value: &str) -> Option<Operation> {
    OPERATIONS
        .iter()
        .copied()
        .find(|operation| operation.action == value || operation.function == value)
}

pub fn cache_key(operation: &str, key_part: &str) -> String {
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    key_part.hash(&mut hasher);
    format!("core-query:v1:{operation}:global:{:016x}", hasher.finish())
}

pub fn cache_pattern(operation: &str) -> String {
    format!("core-query:v1:{operation}:global:*")
}
use std::hash::{Hash, Hasher};
