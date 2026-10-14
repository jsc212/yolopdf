// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick
import QtTest
TestCase {
    name: "DropRouter"
    function test_route_data() {
        return [
            {tag:"between thumbnails",target:"overview",open:true,internal:false,count:3,hover:1,after:false,files:["a.pdf"],action:"insert",at:1,overlay:"Insert pages here"},
            {tag:"after thumbnail",target:"overview",open:true,internal:false,count:3,hover:1,after:true,files:["a.pdf"],action:"insert",at:2,overlay:"Insert pages here"},
            {tag:"below last",target:"overview",open:true,internal:false,count:3,hover:-1,after:false,files:["a.pdf"],action:"insert",at:3,overlay:"Insert pages here"},
            {tag:"several ordered PDFs",target:"overview",open:true,internal:false,count:3,hover:0,after:false,files:["a.pdf","b.pdf"],action:"insert",at:0,overlay:"Insert pages here"},
            {tag:"main opens new tab",target:"main",open:true,internal:false,count:3,hover:-1,after:false,files:["a.pdf"],action:"open",at:3,overlay:"Open as document"},
            {tag:"empty anywhere",target:"overview",open:false,internal:false,count:0,hover:-1,after:false,files:["a.pdf"],action:"open",at:0,overlay:"Open as document"},
            {tag:"internal reorder",target:"overview",open:true,internal:true,count:3,hover:2,after:false,files:[],action:"reorder",at:2,overlay:""},
            {tag:"internal ignored main",target:"main",open:true,internal:true,count:3,hover:0,after:false,files:[],action:"ignore",at:0,overlay:""}
        ]
    }
    function test_route(data) {
        let result=dropRouter.route(data.target,data.open,data.internal,data.count,data.hover,data.after,data.files)
        compare(result.action,data.action);compare(result.at,data.at);compare(result.overlay,data.overlay);compare(result.files,data.files)
    }
}
