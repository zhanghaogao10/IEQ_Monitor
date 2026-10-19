# SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
from typing import Callable

import pytest
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@idf_parametrize('target', ['esp32c3'], indirect=['target'])
def test_ieq_monitor_boot(dut: IdfDut, log_minimum_free_heap_size: Callable[..., None]) -> None:
    dut.expect('IEQ Monitor starting')
    log_minimum_free_heap_size()
