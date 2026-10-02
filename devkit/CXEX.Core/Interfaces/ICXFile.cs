// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿namespace CXEX.Core.Interfaces;

public interface ICXFile
{
    string GetDisplayName();
    void Load(byte[] data);
}