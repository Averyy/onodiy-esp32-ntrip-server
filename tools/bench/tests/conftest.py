# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""pytest configuration for the bench tool tests."""


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "docker: needs the Docker daemon and RUN_DOCKER_TESTS=1 (builds and runs the local test caster image)",
    )
