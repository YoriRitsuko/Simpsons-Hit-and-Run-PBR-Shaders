# Run on Windows with PowerShell 7 and Node 22 or newer on PATH.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

function ConvertToTga([string] $Source, [string] $Destination) {
    if ([IO.Path]::GetExtension($Source) -ieq '.tga') { return $Source }
    $bitmap = [Drawing.Bitmap]::new($Source)
    try {
        $width = $bitmap.Width
        $height = $bitmap.Height
        if ($width -gt 4096 -or $height -gt 4096) { throw 'Resize images to 4096 pixels or smaller.' }
        $size = $width * $height * 4
        $bytes = [byte[]]::new(18 + $size)
        $bytes[2] = 2
        [BitConverter]::GetBytes([uint16]$width).CopyTo($bytes, 12)
        [BitConverter]::GetBytes([uint16]$height).CopyTo($bytes, 14)
        $bytes[16] = 32
        $bytes[17] = 0x28
        $bounds = [Drawing.Rectangle]::new(0, 0, $width, $height)
        $pixels = $bitmap.LockBits($bounds, [Drawing.Imaging.ImageLockMode]::ReadOnly,
                                   [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            for ($y = 0; $y -lt $height; $y++) {
                $row = [IntPtr]::Add($pixels.Scan0, $y * $pixels.Stride)
                [Runtime.InteropServices.Marshal]::Copy($row, $bytes, 18 + $y * $width * 4, $width * 4)
            }
        } finally {
            $bitmap.UnlockBits($pixels)
        }
        [IO.File]::WriteAllBytes($Destination, $bytes)
        return $Destination
    } finally {
        $bitmap.Dispose()
    }
}

function Invoke-PackTextureSet([hashtable] $Fields) {
    $node = Get-Command -Name node.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$node) { throw 'Install Node 22 or newer and make sure node.exe is on PATH.' }
    if (!$Fields.shader) { throw 'Enter the exact shader name from the model.' }
    if (!$Fields.output) { throw 'Choose an output .p3d file.' }
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('pbr-packer-' + [Guid]::NewGuid().ToString('N'))
    New-Item -Path $temp -ItemType Directory | Out-Null
    try {
        $config = @{ shader = $Fields.shader; mode = $Fields.mode; output = [IO.Path]::GetFullPath($Fields.output) }
        foreach ($name in $Fields.Keys) {
            if ($name -in @('shader', 'mode', 'output') -or !$Fields[$name]) { continue }
            $inputFile = (Resolve-Path -LiteralPath $Fields[$name]).Path
            if ($name -in @('model', 'environment')) { $config[$name] = $inputFile; continue }
            $config[$name] = ConvertToTga $inputFile (Join-Path $temp ($name + '.tga'))
        }
        $json = Join-Path $temp 'inputs.json'
        [IO.File]::WriteAllText($json, ($config | ConvertTo-Json))
        $result = & $node.Source (Join-Path $PSScriptRoot 'pack-texture-set.cjs') $json 2>&1
        if ($LASTEXITCODE -ne 0) { throw ($result -join [Environment]::NewLine) }
        return $result -join ' '
    } finally {
        Get-ChildItem -LiteralPath $temp -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
        Remove-Item -LiteralPath $temp
    }
}

$form = [Windows.Forms.Form]::new()
$form.Text = 'PBR Shaders Texture Packer'
$form.ClientSize = [Drawing.Size]::new(810, 615)
$form.MinimumSize = [Drawing.Size]::new(640, 680)
$form.StartPosition = 'CenterScreen'
$form.Font = [Drawing.Font]::new('Segoe UI', 10)
$form.AutoScaleMode = 'Dpi'

$grid = [Windows.Forms.TableLayoutPanel]::new()
$grid.Dock = 'Fill'
$grid.Padding = [Windows.Forms.Padding]::new(16, 13, 16, 12)
$grid.ColumnCount = 3
$grid.RowCount = 16
$grid.AutoScroll = $true
$null = $grid.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Absolute', 195))
$null = $grid.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Percent', 100))
$null = $grid.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Absolute', 95))
$null = $grid.RowStyles.Add([Windows.Forms.RowStyle]::new('Absolute', 45))
for ($i = 1; $i -lt $grid.RowCount; $i++) {
    $null = $grid.RowStyles.Add([Windows.Forms.RowStyle]::new('Absolute', 34))
}
$form.Controls.Add($grid)

$intro = [Windows.Forms.Label]::new()
$intro.Text = 'Pack one material into a Pure3D file. Leave the input model blank to make a separate material pack.'
$intro.Dock = 'Fill'
$grid.Controls.Add($intro, 0, 0)
$grid.SetColumnSpan($intro, 3)

function Add-Label([int] $Row, [string] $Text) {
    $label = [Windows.Forms.Label]::new()
    $label.Text = $Text
    $label.Dock = 'Fill'
    $label.TextAlign = 'MiddleLeft'
    $grid.Controls.Add($label, 0, $Row)
}

Add-Label 1 'Pure3D shader name'
$shader = [Windows.Forms.TextBox]::new()
$shader.Dock = 'Fill'
$shader.MaxLength = 127
$grid.Controls.Add($shader, 1, 1)

Add-Label 2 'Material'
$mode = [Windows.Forms.ComboBox]::new()
$mode.DropDownStyle = 'DropDownList'
$mode.Items.AddRange([object[]]@('PBR', 'Interior'))
$mode.SelectedIndex = 0
$grid.Controls.Add($mode, 1, 2)

$paths = @{}
$imageFilter = 'Texture images (*.png;*.tga;*.bmp;*.jpg)|*.png;*.tga;*.bmp;*.jpg;*.jpeg'
$p3dFilter = 'Pure3D files (*.p3d)|*.p3d'

function Add-Path([int] $Row, [string] $Name, [string] $Label, [string] $Filter, [bool] $Save = $false) {
    Add-Label $Row $Label
    $box = [Windows.Forms.TextBox]::new()
    $box.Dock = 'Fill'
    $paths[$Name] = $box
    $grid.Controls.Add($box, 1, $Row)

    $browse = [Windows.Forms.Button]::new()
    $browse.Text = if ($Save) { 'Save as...' } else { 'Browse...' }
    $browse.Dock = 'Fill'
    $browse.Tag = @{ Box = $box; Filter = $Filter; Save = $Save }
    $box.Tag = $browse
    $browse.Add_Click({
        param($sender, $event)
        $field = $sender.Tag
        $dialog = if ($field.Save) { [Windows.Forms.SaveFileDialog]::new() } else { [Windows.Forms.OpenFileDialog]::new() }
        try {
            $dialog.Filter = $field.Filter
            $dialog.FileName = $field.Box.Text
            if ($field.Save) { $dialog.DefaultExt = 'p3d' }
            if ($dialog.ShowDialog() -eq 'OK') { $field.Box.Text = $dialog.FileName }
        } finally {
            $dialog.Dispose()
        }
    })
    $grid.Controls.Add($browse, 2, $Row)
}

Add-Path 3 'base' 'Base color' $imageFilter
Add-Path 4 'normal' 'Normal map' $imageFilter
Add-Path 5 'orm' 'Packed ORM' $imageFilter
Add-Path 6 'ao' 'Ambient occlusion' $imageFilter
Add-Path 7 'roughness' 'Roughness' $imageFilter
Add-Path 8 'metallic' 'Metallic' $imageFilter
Add-Path 9 'emissive' 'Emissive map' $imageFilter
Add-Path 10 'interior' 'Interior mirror-ball map' $imageFilter
Add-Path 11 'environment' 'Environment reflection' 'Mirror-ball HDR or DDS (*.hdr;*.dds)|*.hdr;*.dds'
Add-Path 12 'model' 'Input model (optional)' $p3dFilter
Add-Path 13 'output' 'Output file' $p3dFilter $true
$paths.interior.Enabled = $false
$paths.interior.Tag.Enabled = $false

$hint = [Windows.Forms.Label]::new()
$hint.Text = 'Use either Packed ORM or the separate AO, roughness, and metallic maps.'
$hint.Dock = 'Fill'
$grid.Controls.Add($hint, 0, 14)
$grid.SetColumnSpan($hint, 3)
$mode.Add_SelectedIndexChanged({
    $enableRoom = $mode.Text -eq 'Interior'
    $paths.interior.Enabled = $enableRoom
    $paths.interior.Tag.Enabled = $enableRoom
    $hint.Text = if ($enableRoom) {
        'Facade alpha marks the window. Room map alpha marks the parts that glow.'
    } else { 'Use either Packed ORM or the separate AO, roughness, and metallic maps.' }
})

$status = [Windows.Forms.Label]::new()
$status.Dock = 'Fill'
$status.TextAlign = 'MiddleLeft'
$status.AutoEllipsis = $true
$grid.Controls.Add($status, 0, 15)
$grid.SetColumnSpan($status, 2)

$pack = [Windows.Forms.Button]::new()
$pack.Text = 'Pack'
$pack.Dock = 'Fill'
$grid.Controls.Add($pack, 2, 15)
$form.AcceptButton = $pack
$pack.Add_Click({
    $pack.Enabled = $false
    $status.Text = 'Packing textures...'
    $form.Refresh()
    try {
        $fields = @{ shader = $shader.Text.Trim(); mode = $mode.Text }
        foreach ($name in $paths.Keys) {
            if ($paths[$name].Enabled) { $fields[$name] = $paths[$name].Text.Trim() }
        }
        $status.Text = Invoke-PackTextureSet $fields
    } catch {
        $status.Text = 'Packing failed.'
        $null = [Windows.Forms.MessageBox]::Show($form, $_.Exception.Message, 'PBR Shaders Texture Packer', 'OK', 'Error')
    } finally {
        $pack.Enabled = $true
    }
})

[void]$form.ShowDialog()
$form.Dispose()
