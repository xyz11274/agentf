"""Create actual office files; delivery to another person is a separate action."""
from datetime import datetime, timezone
from html import escape
from io import BytesIO, StringIO
import csv
import importlib
import sys

from .browser import RUNTIME


def dependency(name):
    if str(RUNTIME) not in sys.path:
        sys.path.insert(0, str(RUNTIME))
    return importlib.import_module(name)


def cell(value):
    # User-supplied spreadsheet text is data, never an executable formula.
    if isinstance(value, str) and value.startswith(('=', '+', '-', '@')):
        return "'" + value
    return value


def render(args, title, content):
    fmt = args.get('format', 'md')
    stream = BytesIO()
    if fmt in ('md', 'txt', 'html'):
        text = content if fmt != 'html' else '<!doctype html><meta charset="utf-8"><title>'+escape(title)+'</title><h1>'+escape(title)+'</h1><pre>'+escape(content)+'</pre>'
        return text.encode(), fmt, {'md':'text/markdown','txt':'text/plain','html':'text/html'}[fmt]
    if fmt == 'docx':
        doc = dependency('docx').Document()
        doc.add_heading(title, 0)
        for line in content.splitlines():
            if line.startswith('#'):
                depth = min(len(line)-len(line.lstrip('#')), 4)
                doc.add_heading(line.lstrip('# ').strip(), depth)
            else:
                doc.add_paragraph(line)
        doc.save(stream)
        mime = 'application/vnd.openxmlformats-officedocument.wordprocessingml.document'
    elif fmt in ('xlsx', 'csv'):
        sheets = args.get('sheets')
        if not sheets:
            raise ValueError('表格导出需要 sheets（name、rows），不能把普通段落冒充表格')
        if fmt == 'csv':
            if len(sheets) != 1:
                raise ValueError('CSV 只支持一张表；多表请使用 xlsx')
            text = StringIO(newline='')
            writer = csv.writer(text)
            writer.writerows([[cell(v) for v in row] for row in sheets[0]['rows']])
            return text.getvalue().encode('utf-8-sig'), 'csv', 'text/csv'
        book = dependency('openpyxl').Workbook()
        book.remove(book.active)
        for index, sheet in enumerate(sheets):
            name = sheet.get('name') or f'Sheet{index+1}'
            page = book.create_sheet(name[:31])
            for row in sheet['rows']:
                page.append([cell(v) for v in row])
            page.freeze_panes = 'A2'
            page.auto_filter.ref = page.dimensions
        book.save(stream)
        mime = 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet'
    elif fmt == 'pptx':
        pptx = dependency('pptx')
        deck = pptx.Presentation()
        slides = args.get('slides') or [{'title':title,'content':content}]
        for item in slides:
            slide = deck.slides.add_slide(deck.slide_layouts[1])
            slide.shapes.title.text = item.get('title', title)
            slide.placeholders[1].text = item.get('content', '')
        deck.save(stream)
        mime = 'application/vnd.openxmlformats-officedocument.presentationml.presentation'
    elif fmt == 'pdf':
        dependency('reportlab')
        from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer
        from reportlab.lib.styles import ParagraphStyle
        from reportlab.pdfbase import pdfmetrics
        from reportlab.pdfbase.cidfonts import UnicodeCIDFont
        pdfmetrics.registerFont(UnicodeCIDFont('STSong-Light'))
        style = ParagraphStyle('Chinese', fontName='STSong-Light', fontSize=11, leading=17)
        story = [Paragraph(escape(title), style), Spacer(1, 14)]
        for line in content.splitlines():
            story.append(Paragraph(escape(line) or '&#160;', style))
        SimpleDocTemplate(stream).build(story)
        mime = 'application/pdf'
    else:
        raise ValueError('unsupported office format')
    return stream.getvalue(), fmt, mime


def calendar(args, identifier):
    """One actual calendar event. No ambiguous local timestamps or fake invites."""
    start = datetime.fromisoformat(args['start_time'].replace('Z', '+00:00'))
    end = datetime.fromisoformat(args['end_time'].replace('Z', '+00:00'))
    if start.tzinfo is None or end.tzinfo is None or end <= start:
        raise ValueError('日程起止时间需要时区，结束必须晚于开始')
    def clean(text):
        return str(text).replace('\\','\\\\').replace('\r','').replace('\n','\\n').replace(';','\\;').replace(',','\\,')
    def stamp(value):
        return value.astimezone(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    lines = ['BEGIN:VCALENDAR','VERSION:2.0','PRODID:-//AgentFlow//Business Calendar//ZH',
             'BEGIN:VEVENT',f'UID:{identifier}@agentflow.local',f'DTSTAMP:{stamp(datetime.now(timezone.utc))}',
             f'DTSTART:{stamp(start)}',f'DTEND:{stamp(end)}','SUMMARY:'+clean(args['title']),
             'DESCRIPTION:'+clean(args.get('content','')),'LOCATION:'+clean(args.get('location','')),
             'END:VEVENT','END:VCALENDAR']
    folded=[]
    for line in lines:
        part=''
        for char in line:
            if len((part+char).encode())>73:
                folded.append(part);part=' '+char
            else:
                part+=char
        folded.append(part)
    return ('\r\n'.join(folded)+'\r\n').encode()
