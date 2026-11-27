# Creates Office samples (docx doc odt, xlsx xls ods, pptx ppt odp) in build\samples\office through Office COM.
# Needs Microsoft Office; runs it hidden. Idempotent.
$dir = Join-Path $PSScriptRoot '..\build\samples\office'
New-Item -ItemType Directory -Force $dir | Out-Null
$dir = (Resolve-Path $dir).Path

if (-not (Test-Path "$dir\doc.docx")) {
    $w = New-Object -ComObject Word.Application
    $d = $w.Documents.Add()
    $s = $d.Content
    $s.Text = "Quarterly report"
    $s.Style = -2  # wdStyleHeading1
    $s.InsertParagraphAfter()
    $r = $d.Paragraphs.Last.Range
    $r.Text = "This paragraph has bold text, italic text and 5 * 3 = 15 [brackets] #hash."
    $r.Style = -1  # normal
    $b = $r.Duplicate; $b.Find.Execute("bold text") | Out-Null; $b.Bold = 1
    $i = $r.Duplicate; $i.Find.Execute("italic text") | Out-Null; $i.Italic = 1
    $r.InsertParagraphAfter()
    $r = $d.Paragraphs.Last.Range; $r.Text = "Details"; $r.Style = -3  # Heading 2
    $r.InsertParagraphAfter()
    foreach ($t in "First point", "Second point", "Third point") {
        $r = $d.Paragraphs.Last.Range; $r.Text = $t; $r.Style = -49  # List Bullet
        $r.InsertParagraphAfter()
    }
    $r = $d.Paragraphs.Last.Range; $r.Style = -1
    $tbl = $d.Tables.Add($r, 3, 3)
    $cells = @(("Name", "Qty", "Price"), ("Apple", "3", "1.20"), ("Pear", "5", "0.80"))
    for ($y = 0; $y -lt 3; $y++) { for ($x = 0; $x -lt 3; $x++) { $tbl.Cell($y + 1, $x + 1).Range.Text = $cells[$y][$x] } }
    $d.Content.InsertParagraphAfter()
    $d.Paragraphs.Last.Range.Text = "The end."
    $d.SaveAs2("$dir\doc.docx", 16)
    $d.SaveAs2("$dir\doc.doc", 0)
    $d.SaveAs2("$dir\doc.odt", 23)
    $d.Close(0); $w.Quit()
}

if (-not (Test-Path "$dir\sheet.xlsx")) {
    $x = New-Object -ComObject Excel.Application
    $x.DisplayAlerts = $false
    $wb = $x.Workbooks.Add()
    $ws = $wb.Worksheets.Item(1)
    $ws.Name = "Sales"
    $ws.Range("A1:E1").Value2 = [object[]]("Item", "Qty", "Price", "Date", "Ok")
    $ws.Range("A2").Value2 = "Apple"; $ws.Range("B2").Value2 = [double]3; $ws.Range("C2").Value2 = [double]1.2
    $ws.Range("A3").Value2 = "Pear, green"; $ws.Range("B3").Value2 = [double]5; $ws.Range("C3").Value2 = [double]0.8
    $ws.Range("D2").Value2 = [double]46037; $ws.Range("D3").Value2 = [double]46054
    $ws.Range("E2").Formula = "=TRUE()"; $ws.Range("E3").Formula = "=FALSE()"
    $ws.Range("D2:D3").NumberFormat = "yyyy-mm-dd"
    $ws.Range("F1").Value2 = "Total"
    $ws.Range("F2").Formula = "=B2*C2"
    $ws.Range("G2").Formula = '=A2&"!"'
    for ($y = 4; $y -le 200; $y++) { $ws.Cells.Item($y, 1).Value2 = "Row $y"; $ws.Cells.Item($y, 2).Value2 = [double]($y * 1.5) }
    $wb.Worksheets.Add([Type]::Missing, $ws) | Out-Null
    $wb.Worksheets.Item(1).Activate()
    $wb.SaveAs("$dir\sheet.xlsx", 51)
    $wb.SaveAs("$dir\sheet.xls", 56)
    $wb.SaveAs("$dir\sheet.ods", 60)
    $wb.Close($false); $x.Quit()
}

# A graded-lab workbook like a real one: merged labels, fills (rgb and theme + tint), borders, wrapped text,
# number formats, overflow, a picture and a chart, 25 sheets (the tab strip overflows; a late one is active).
if (-not (Test-Path "$dir\lab.xlsx")) {
    $x = New-Object -ComObject Excel.Application
    $x.DisplayAlerts = $false
    try {
        $wb = $x.Workbooks.Add()
        $ws = $wb.Worksheets.Item(1)
        $orange = 0x96C8FF  # BGR: FFC896
        foreach ($c in @(("A", 18), ("B", 10), ("C", 34), ("D", 10), ("E", 10), ("F", 22), ("G", 34), ("H", 16))) { $ws.Columns.Item($c[0]).ColumnWidth = $c[1] }
        $ws.Range("A1").Value2 = "Student"; $ws.Range("B1").Value2 = "Rachel Yu"; $ws.Range("C1").Value2 = "Lab"; $ws.Range("D1").Value2 = [double]2
        $ws.Rows.Item(1).RowHeight = 36
        $ws.Range("B1,D1").Interior.Color = $orange
        $ws.Range("B1").Font.Color = 0x3C64C8  # BGR: C8643C
        $ws.Range("A2").Value2 = "TA sheet for grading labs for 2CD4 in 2025"
        $ws.Range("C3").Value2 = "Only change the orange input cells"; $ws.Range("C4").Value2 = "Results appear in output cells"
        $ws.Range("C3:C4").HorizontalAlignment = -4152  # right
        $ws.Range("D3").Value2 = "like this"; $ws.Range("D3").Interior.Color = $orange
        $ws.Range("D4").Value2 = "like this"; $ws.Range("D4").Font.Bold = $true
        $ws.Range("D3:D4").Borders.LineStyle = 1
        $ws.Range("A5").Value2 = "Part 1: Potential Marks"; $ws.Range("A5").Font.Bold = $true; $ws.Range("A5").Font.Underline = 2
        $ws.Range("A6").Value2 = "Update Potential Marks in cells below based on percent of their submitted work that is correct."
        $ws.Range("A7").Value2 = "To be correct, work must be presented clearly in their submitted Word file."
        $ws.Range("A10:E10").Value2 = [object[]]("Problem", "Part", "Description", "Correct?", "Total")
        $ws.Range("A10:E10").Font.Bold = $true
        $rows = @(
            @(11, 14, "L9 (Work & Kinetic Energy)", @("a", "b", "c", "Summary"), @('Work done by "W"', 'Work done by "T"', "Change in kinetic energy", "Units & closing statement(s)"), @(1, 1, 1, 1)),
            @(15, 16, "L10 (Gravitational Potential)", @("a", "Summary"), @("Speed of the satellite (vB)", "Units & closing statement(s)"), @(1, 1)),
            @(17, 19, "L11 (Main Lecture Problem)", @("a", "b", "Summary"), @("Speed of Springzan after the collision", "Speed of Jane after the collision", "Units & closing statement(s)"), @(1, 1, 1)),
            @(20, 21, "L11 (Tobaggan)", @("1", "Summary"), @("Final speed of the tobaggan (v_t)", "Units & closing statement(s)"), @(1, 1)),
            @(22, 25, "L12 (Inf. Worlds Colliding)", @("a.1", "a.2", "b", "Summary"), @('Correct final velocity for ball "a" plotted against coefficient of restitution?', 'Correct final velocity for ball "b" plotted against coefficient of restitution?', "Correct fraction of the system's initial KE remainig plotted against coefficient of restitution?", "Units & closing statement(s)"), @(1, 1, 0.5, 1)),
            @(26, 27, "L13 (Rocket Thrust)", @("a", "Summary"), @("Determine the maximum height of the rocket", "Units & closing statement(s)"), @(1, 1)))
        foreach ($g in $rows) {
            $r0 = $g[0]; $r1 = $g[1]
            for ($k = 0; $k -le $r1 - $r0; $k++) {
                $r = $r0 + $k
                $ws.Cells.Item($r, 2).Value2 = $g[3][$k]; $ws.Cells.Item($r, 3).Value2 = $g[4][$k]; $ws.Cells.Item($r, 4).Value2 = [double]$g[5][$k]
            }
            $a = $ws.Range("A${r0}:A${r1}"); $a.Merge(); $a.Value2 = $g[2]; $a.WrapText = $true; $a.Font.Bold = $true
            $a.HorizontalAlignment = -4108; $a.VerticalAlignment = -4108
            $e = $ws.Range("E${r0}:E${r1}"); $e.Merge(); $e.Formula = "=AVERAGE(D${r0}:D${r1})"; $e.Font.Bold = $true
            $e.HorizontalAlignment = -4108; $e.VerticalAlignment = -4108
            if ($ws.Range("E$r0").Value2 -lt 1) { $e.Interior.Color = 0x99E6FF; $e.Font.Color = 0x0060B0 }  # yellow, brown text
            else { $e.Interior.ThemeColor = 10; $e.Interior.TintAndShade = 0.6; $e.Font.Color = 0x006100 }  # accent 6 tint, green text
        }
        $ws.Range("B11:B27").HorizontalAlignment = -4152
        $ws.Range("C11:C27").WrapText = $true
        $ws.Range("D11:D27").Interior.Color = $orange
        $ws.Range("D11:E27").NumberFormat = "0.00"
        $ws.Range("A10:E27").Borders.LineStyle = 1
        $ws.Range("A10:E10").Borders.Item(9).Weight = -4138  # medium bottom edge under the header
        $ws.Range("F11").Value2 = "-260 J"; $ws.Range("F12").Value2 = "354 J"; $ws.Range("F13").Value2 = "93.1 J"
        $ws.Range("F15").Value2 = "9670 m/s or 34.8 Mm/h"; $ws.Range("F17").Value2 = "Vs1/3"; $ws.Range("G17").Value2 = "3.13m/s left"
        $ws.Range("F26").Value2 = "Around 148 to 150 km"
        $ws.Range("G10").Value2 = "Score"; $ws.Range("G10").Font.Bold = $true
        $ws.Range("G11").Formula = "=AVERAGE(E11:E27)"; $ws.Range("G11").NumberFormat = "0.0%"
        $ws.Range("G12").Value2 = [double]46037.75; $ws.Range("G12").NumberFormat = "dddd, mmmm d, yyyy h:mm AM/PM"
        $ws.Range("G13").Value2 = [double]46037; $ws.Range("G13").NumberFormat = "yyyy-mm-dd"
        $ws.Range("G14").Value2 = [double]-1234567.891; $ws.Range("G14").NumberFormat = '$#,##0.00_);($#,##0.00)'
        $ws.Range("G15").Value2 = [double]0.000123; $ws.Range("G15").NumberFormat = "0.00E+00"
        $ws.Range("G16").Value2 = [double](2 / 3)
        $ws.Range("H10").Value2 = "Notes"; $ws.Range("H10").Font.Italic = $true; $ws.Range("H10").Font.Color = 0x0000C0
        $ws.Range("H11").Value2 = "struck"; $ws.Range("H11").Font.Strikethrough = $true
        $ws.Range("H12").Value2 = "centred overflow text"; $ws.Range("H12").HorizontalAlignment = -4108
        $ws.Range("A28").Value2 = "Part 2: Graphs"; $ws.Range("A28").Font.Bold = $true; $ws.Range("A28").Font.Size = 14
        for ($k = 0; $k -lt 6; $k++) { $ws.Cells.Item(29 + $k, 1).Value2 = [double]($k / 5); $ws.Cells.Item(29 + $k, 2).Value2 = [double](3 - 5 * $k / 5) }
        $pic = $ws.Range("G19")
        $ws.Shapes.AddPicture((Resolve-Path "$dir\..\photo-1920.jpg").Path, 0, 1, $pic.Left, $pic.Top, 300, 170) | Out-Null
        $cr = $ws.Range("C29")
        $ch = $ws.Shapes.AddChart2(227, 4, $cr.Left, $cr.Top, 300, 180)  # line chart
        $ch.Chart.SetSourceData($ws.Range("A29:B34"))
        $names = "Rachel Yu", "Shayan Siddiqui", "Owen Bungard", "Tony Tan", "Djordje Spasovic", "Isaac Ho", "Ira Sharda",
            "Christine Wong", "Maya Patel", "Liam O'Brien", "Sofia Rossi", "Noah Kim", "Ava Johnson", "Ethan Nguyen",
            "Chloe Martin", "Lucas Silva", "Mia Chen", "Jack Wilson", "Zoe Brown", "Omar Haddad", "Emma Davis",
            "Ryan Lee", "Hannah Muller", "Daniel Park", "Grace Taylor"
        $ws.Name = $names[0]
        for ($k = 1; $k -lt $names.Count; $k++) {
            $ws.Copy([Type]::Missing, $wb.Worksheets.Item($wb.Worksheets.Count))
            $s = $wb.Worksheets.Item($wb.Worksheets.Count); $s.Name = $names[$k]; $s.Range("B1").Value2 = $names[$k]
        }
        $wb.Worksheets.Item(18).Activate()
        $wb.Worksheets.Item(18).Range("D24").Value2 = [double]0.25
        $wb.SaveAs("$dir\lab.xlsx", 51)
        $wb.Worksheets.Item(1).ExportAsFixedFormat(0, "$dir\lab-excel.pdf")  # how Excel prints it, for comparison
        $wb.Close($false)
    } finally { $x.Quit(); [void][Runtime.InteropServices.Marshal]::ReleaseComObject($x) }
}

# 50k rows x 20 columns (text, numbers, dates), header row frozen: open-time test for big sheets.
if (-not (Test-Path "$dir\big.xlsx")) {
    $x = New-Object -ComObject Excel.Application
    $x.DisplayAlerts = $false
    try {
        $wb = $x.Workbooks.Add()
        $ws = $wb.Worksheets.Item(1)
        $ws.Name = "Data"
        $head = [object[]](1..20 | ForEach-Object { "Column $_" })
        $ws.Range("A1:T1").Value2 = $head
        $ws.Range("A1:T1").Font.Bold = $true; $ws.Range("A1:T1").Interior.Color = 0xF2E6DD
        $ws.Range("A2:A50000").Formula = '="Item "&ROW()'
        $ws.Range("B2:B50000").Formula = '=CHOOSE(MOD(ROW(),5)+1,"North","South","East","West","Central")'
        $ws.Range("C2:C50000").Formula = '=DATE(2020,1,1)+MOD(ROW()*7,2000)'
        $ws.Range("D2:Q50000").Formula = '=ROUND(RAND()*10000,2)'
        $ws.Range("R2:R50000").Formula = '=ROW()'
        $ws.Range("S2:S50000").Formula = '=RAND()'
        $ws.Range("T2:T50000").Formula = '=IF(MOD(ROW(),3)=0,"yes","no")'
        $ws.Range("A2:T50000").Value2 = $ws.Range("A2:T50000").Value2
        $ws.Range("C2:C50000").NumberFormat = "yyyy-mm-dd"
        $ws.Range("D2:Q50000").NumberFormat = "#,##0.00"
        $ws.Range("S2:S50000").NumberFormat = "0.0%"
        $ws.Range("A2").Select() | Out-Null
        $x.ActiveWindow.FreezePanes = $false
        $ws.Range("B2").Select() | Out-Null
        $x.ActiveWindow.FreezePanes = $true
        $wb.SaveAs("$dir\big.xlsx", 51)
        $wb.Close($false)
    } finally { $x.Quit(); [void][Runtime.InteropServices.Marshal]::ReleaseComObject($x) }
}

if (-not (Test-Path "$dir\slides.pptx")) {
    $p = New-Object -ComObject PowerPoint.Application
    $pr = $p.Presentations.Add(0)
    $s1 = $pr.Slides.Add(1, 1)  # ppLayoutTitle
    $s1.Shapes.Item(1).TextFrame.TextRange.Text = "Flick deck"
    $s1.Shapes.Item(2).TextFrame.TextRange.Text = "A subtitle"
    $s2 = $pr.Slides.Add(2, 2)  # ppLayoutText
    $s2.Shapes.Item(1).TextFrame.TextRange.Text = "Agenda"
    $s2.Shapes.Item(2).TextFrame.TextRange.Text = "Fast`rLight`rNative"
    $s2.Shapes.Item(2).TextFrame.TextRange.Paragraphs(2).IndentLevel = 2
    $box = $s2.Shapes.AddTextbox(1, 50, 400, 300, 40)
    $box.TextFrame.TextRange.Text = "A free text box"
    $s3 = $pr.Slides.Add(3, 12)  # blank
    $t = $s3.Shapes.AddTable(2, 2).Table
    $t.Cell(1, 1).Shape.TextFrame.TextRange.Text = "Key"; $t.Cell(1, 2).Shape.TextFrame.TextRange.Text = "Value"
    $t.Cell(2, 1).Shape.TextFrame.TextRange.Text = "Speed"; $t.Cell(2, 2).Shape.TextFrame.TextRange.Text = "10 ms"
    $pr.SaveAs("$dir\slides.pptx", 24)
    $pr.SaveAs("$dir\slides.ppt", 1)
    $pr.SaveAs("$dir\slides.odp", 35)
    $pr.Close(); $p.Quit()
}
Get-ChildItem $dir | Select-Object Name, Length
