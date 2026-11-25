from strictdoc.core.project_config import ProjectConfig


def create_config() -> ProjectConfig:
    return ProjectConfig(
        project_title="StrataForge (STRATA-20) Formal Requirements Specification",
        dir_for_sdoc_assets="assets",
        include_doc_paths=[
            "requirements/**",
        ],
        exclude_doc_paths=[
            "build/**",
            "docs/**",
        ],
        features=[
            "TABLE_SCREEN",
            "TRACEABILITY_SCREEN",
            "DEEP_TRACEABILITY_SCREEN",
            "PROJECT_STATISTICS_SCREEN",
            "REQUIREMENT_TO_SOURCE_TRACEABILITY",
        ],
    )
