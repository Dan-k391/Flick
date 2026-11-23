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
