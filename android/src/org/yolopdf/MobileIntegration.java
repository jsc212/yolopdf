// SPDX-License-Identifier: AGPL-3.0-only
package org.yolopdf;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.pdf.PdfRenderer;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintManager;
import android.print.pdf.PrintedPdfDocument;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.UUID;

public final class MobileIntegration {
    private static final ArrayDeque<String> documents = new ArrayDeque<>();
    private MobileIntegration() {}
    public static synchronized void receiveIntent(Intent intent) {
        if (intent == null || !"application/pdf".equals(intent.getType())) return;
        Uri uri = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction())) uri = intent.getData();
        else if (Intent.ACTION_SEND.equals(intent.getAction())) uri = (Uri)intent.getParcelableExtra(Intent.EXTRA_STREAM);
        if (uri != null && ("content".equals(uri.getScheme()) || "file".equals(uri.getScheme()))) {
            if (documents.size() < 32) documents.add(uri.toString());
        }
    }
    public static synchronized String takeDocument() { return documents.isEmpty() ? "" : documents.remove(); }
    public static String copyImage(Context context,String source) {
        try {
            File folder = new File(context.getCacheDir(), "clipboard");
            if (!folder.isDirectory() && !folder.mkdirs()) return "Cannot create clipboard cache";
            // Keep recent files so clipboard receivers can read asynchronously.
            File[] old = folder.listFiles();
            if (old != null) for (File f:old) if (System.currentTimeMillis()-f.lastModified()>86400000L) f.delete();
            File image = new File(folder,UUID.randomUUID()+".png");
            try (FileInputStream in = new FileInputStream(source); FileOutputStream out = new FileOutputStream(image)) {
                byte[] bytes = new byte[65536]; for(int n;(n=in.read(bytes))>=0;) if(n>0) out.write(bytes,0,n);
            }
            Uri uri = Uri.parse("content://"+context.getPackageName()+".images/"+image.getName());
            ClipboardManager clipboard = (ClipboardManager)context.getSystemService(Context.CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(ClipData.newUri(context.getContentResolver(),"yolopdf image",uri));
            return "";
        } catch (Exception error) { return "Image clipboard failed: "+error.getMessage(); }
    }
    public static void print(Activity activity,String source,String title,boolean fit) {
        activity.runOnUiThread(() -> {
            PrintManager manager = (PrintManager)activity.getSystemService(Context.PRINT_SERVICE);
            manager.print(title,new Adapter(activity,new File(source),title,fit),null);
        });
    }
    private static final class Adapter extends PrintDocumentAdapter {
        private final Activity activity;
        private final File source;
        private final String title;
        private final boolean fit;
        private PrintAttributes attributes;
        Adapter(Activity a,File s,String t,boolean f) { activity=a;source=s;title=t;fit=f; }
        @Override public void onLayout(PrintAttributes old,PrintAttributes next,CancellationSignal cancel,LayoutResultCallback callback,Bundle extras) {
            attributes=next;
            if(cancel.isCanceled()){callback.onLayoutCancelled();return;}
            try(ParcelFileDescriptor fd=ParcelFileDescriptor.open(source,ParcelFileDescriptor.MODE_READ_ONLY);PdfRenderer renderer=new PdfRenderer(fd)) {
                callback.onLayoutFinished(new PrintDocumentInfo.Builder(title).setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT).setPageCount(renderer.getPageCount()).build(),!next.equals(old));
            } catch(Exception error) { callback.onLayoutFailed(error.getMessage()); }
        }
        @Override public void onWrite(PageRange[] ranges,ParcelFileDescriptor destination,CancellationSignal cancel,WriteResultCallback callback) {
            new Thread(() -> {
                try(ParcelFileDescriptor fd=ParcelFileDescriptor.open(source,ParcelFileDescriptor.MODE_READ_ONLY);
                    PdfRenderer renderer=new PdfRenderer(fd);
                    PrintedPdfDocument document=new PrintedPdfDocument(activity,attributes);
                    FileOutputStream output=new ParcelFileDescriptor.AutoCloseOutputStream(destination)) {
                    ArrayList<PageRange> written=new ArrayList<>();
                    for(int i=0;i<renderer.getPageCount();i++) {
                        boolean selected=false;for(PageRange range:ranges) if(i>=range.getStart()&&i<=range.getEnd())selected=true;
                        if(!selected)continue;if(cancel.isCanceled()){callback.onWriteCancelled();return;}
                        try(PdfRenderer.Page input=renderer.openPage(i)) {
                            android.graphics.pdf.PdfDocument.Page page=document.startPage(i);
                            Rect content=page.getInfo().getContentRect();
                            float scale=fit?Math.min((float)content.width()/input.getWidth(),(float)content.height()/input.getHeight()):1;
                            float width=input.getWidth()*scale,height=input.getHeight()*scale;
                            int dpi=Math.min(300,Math.min(attributes.getResolution().getHorizontalDpi(),attributes.getResolution().getVerticalDpi()));
                            float raster=(float)Math.min(dpi/72.0,Math.sqrt(16.0*1024*1024/(width*height)));
                            Bitmap bitmap=Bitmap.createBitmap(Math.max(1,(int)Math.ceil(width*raster)),Math.max(1,(int)Math.ceil(height*raster)),Bitmap.Config.ARGB_8888);
                            bitmap.eraseColor(Color.WHITE);Matrix matrix=new Matrix();matrix.setScale(scale*raster,scale*raster);
                            input.render(bitmap,null,matrix,PdfRenderer.Page.RENDER_MODE_FOR_PRINT);
                            float x=fit?content.left+(content.width()-width)/2:content.left;
                            float y=fit?content.top+(content.height()-height)/2:content.top;
                            page.getCanvas().drawBitmap(bitmap,null,new RectF(x,y,x+width,y+height),null);bitmap.recycle();
                            document.finishPage(page);written.add(new PageRange(i,i));
                        }
                    }
                    document.writeTo(output);callback.onWriteFinished(written.toArray(new PageRange[0]));
                }catch(Exception error){callback.onWriteFailed(error.getMessage());}
            },"yolopdf-print").start();
        }
        @Override public void onFinish() { source.delete();File parent=source.getParentFile();if(parent!=null)parent.delete(); }
    }
}
