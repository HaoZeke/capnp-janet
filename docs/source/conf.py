project = "capnp-janet"
copyright = "2026, Rohit Goswami"
author = "Rohit Goswami"
release = "0.2.4"

extensions = [
    "sphinx_copybutton",
    "sphinx_design",
    "myst_parser",
    "sphinxcontrib.mermaid",
]

templates_path = ["_templates"]
exclude_patterns = ["_build"]

myst_enable_extensions = ["colon_fence", "deflist"]

html_theme = "shibuya"
html_static_path = ["_static"]
html_extra_path = ["llms.txt"]
html_title = "capnp-janet documentation"
html_baseurl = "https://capnp-janet.rgoswami.me/"

html_context = {
    "source_type": "github",
    "source_user": "HaoZeke",
    "source_repo": "capnp-janet",
    "source_version": "main",
    "source_docs_path": "/docs/orgmode/",
}

html_sidebars = {
    "**": [
        "sidebars/localtoc.html",
        "sidebars/repo-stats.html",
        "sidebars/edit-this-page.html",
    ],
}

html_theme_options = {
    "github_url": "https://github.com/HaoZeke/capnp-janet",
    "accent_color": "orange",
    "dark_code": True,
    "globaltoc_expand_depth": 1,
    "nav_links": [
        {"title": "Install", "url": "install", "summary": "pixi, Meson, embed"},
        {"title": "Tutorial", "url": "tutorial", "summary": "First buffer and AddressBook"},
        {"title": "Architecture", "url": "architecture", "summary": "C core, Janet module, packs"},
        {"title": "Wire", "url": "wire", "summary": "Packed, canonical, evolution"},
        {
            "title": "Ecosystem",
            "children": [
                {
                    "title": "c-capnproto",
                    "url": "https://c-capnproto.rgoswami.me",
                    "summary": "Pure C runtime and capnpc-c",
                    "external": True,
                },
                {
                    "title": "capnp-fortran",
                    "url": "https://capnp-fortran.rgoswami.me",
                    "summary": "F2018 runtime, plugin, RPC",
                    "external": True,
                },
                {
                    "title": "capnp-ts",
                    "url": "https://capnp-ts.rgoswami.me",
                    "summary": "TypeScript wire runtime and capnpc-ts",
                    "external": True,
                },
                {
                    "title": "Cap'n Proto",
                    "url": "https://capnproto.org",
                    "summary": "The reference C++ implementation and the wire spec",
                    "external": True,
                },
            ],
        },
        {"title": "GitHub", "url": "https://github.com/HaoZeke/capnp-janet", "external": True},
    ],
}

copybutton_prompt_text = r">>> |\.\.\. |\$ |In \[\d*\]: | {2,5}\.\.\.: | {5,8}: "
copybutton_prompt_is_regexp = True
copybutton_exclude = ".linenos, .gp, .go"
